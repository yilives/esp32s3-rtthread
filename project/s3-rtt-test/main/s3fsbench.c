/*
 * FAT benchmark for the ESP32-S3 RT-Thread image.
 *
 * Why this exists
 * ---------------
 * "Make the filesystem faster" is not actionable without numbers, and the numbers
 * that matter here are not the ones a desktop benchmark would produce. This image
 * runs FAT on a wear-levelling layer over SPI NOR flash, so the cost is dominated
 * by erase granularity and write amplification, not by CPU time:
 *
 *   - a 4 KB logical sector means each small file update rewrites 4 KB
 *   - every wl write is preceded by a wl erase (diskio_wl.c:50), so a "write" is
 *     really an erase plus a program
 *   - a 512-byte sector cuts the amplification for small files 8x, but forces a
 *     read-modify-write of the 4 KB physical page, which measured 70x slower
 *
 * So this measures the operations the application actually performs: bulk write,
 * bulk read, and a burst of small files. It reports milliseconds derived from
 * esp_timer_get_time(), not cycles, because the interesting cost is waiting for
 * flash.
 *
 * What the numbers said (ESP32-S3, 1 MB partition, 4 KB sectors)
 * --------------------------------------------------------------
 *   bulk write 256 KB      1070 ms      239 KB/s
 *   bulk read  256 KB        29 ms     8827 KB/s
 *   create 32 x 64 B       8539 ms       3 files/s   (later 5297 ms after tuning)
 *   erase per small file   ~5 x 4 KB     ~21 KB erased for 64 bytes stored
 *
 * Design notes
 * ------------
 * - `esp_timer_get_time()` rather than rt_tick_get(): the tick is 1 ms and one
 *   small-file create is often faster than that.
 * - No 64-bit printf. rt_kprintf's %lld path is not guaranteed here, so every
 *   duration is converted to milliseconds in a uint32_t before printing.
 * - The test files are always removed. The benchmark never formats and never
 *   touches the partition table, so it is safe on a board holding real data.
 * - Sizes are small on purpose: the partition is 1 MB, and a benchmark that fills
 *   it would change the thing it measures.
 * - Flash operations are counted with --wrap (see main/CMakeLists.txt). Timing
 *   alone cannot distinguish "60 sector programs" from "2 programs plus a block
 *   relocation", and those need opposite fixes.
 */

#include <rtthread.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

#define BENCH_MOUNT     "/storage"
#define BENCH_FILE      BENCH_MOUNT "/bench.bin"
#define BENCH_TMP       BENCH_MOUNT "/bench%03d.tmp"

/* 256 KB of payload through a 4 KB staging buffer: big enough that per-call
 * overhead stops dominating, small enough to fit the 1 MB partition alongside
 * whatever the application already stored. */
#define BENCH_CHUNK     4096
#define BENCH_CHUNKS    64
#define BENCH_TOTAL     (BENCH_CHUNK * BENCH_CHUNKS)

/* 32 files is enough to measure the per-file cost (directory entry + cluster
 * allocation + FAT update) without turning the command into a minute-long run. */
#define BENCH_SMALL_N   32
#define BENCH_SMALL_LEN 64

static rt_uint8_t s_bench_buf[BENCH_CHUNK];

static int s3_bench_run(rt_bool_t do_write, rt_bool_t do_read, rt_bool_t do_small);

/* --------------------------------------------------------------- counters -- */

/*
 * Wrap the two entry points every layer above wear_levelling funnels through.
 * --wrap redirects them at link time (see main/CMakeLists.txt); __real_* is the
 * original.
 */
extern esp_err_t __real_esp_flash_write(void *chip, const void *buffer,
                                        uint32_t address, uint32_t length);
extern esp_err_t __real_esp_flash_erase_region(void *chip, uint32_t start,
                                               uint32_t size);

static volatile rt_atomic_t s_flash_writes;
static volatile rt_atomic_t s_flash_write_bytes;
static volatile rt_atomic_t s_flash_erases;
static volatile rt_atomic_t s_flash_erase_bytes;

esp_err_t __wrap_esp_flash_write(void *chip, const void *buffer,
                                 uint32_t address, uint32_t length)
{
    rt_atomic_add(&s_flash_writes, 1);
    rt_atomic_add(&s_flash_write_bytes, length);
    return __real_esp_flash_write(chip, buffer, address, length);
}

esp_err_t __wrap_esp_flash_erase_region(void *chip, uint32_t start, uint32_t size)
{
    rt_atomic_add(&s_flash_erases, 1);
    rt_atomic_add(&s_flash_erase_bytes, size);
    return __real_esp_flash_erase_region(chip, start, size);
}

static void bench_counters_reset(void)
{
    rt_atomic_store(&s_flash_writes, 0);
    rt_atomic_store(&s_flash_write_bytes, 0);
    rt_atomic_store(&s_flash_erases, 0);
    rt_atomic_store(&s_flash_erase_bytes, 0);
}

static void bench_counters_print(const char *what)
{
    rt_kprintf("s3fsbench: %s -> %u writes (%u B), %u erases (%u B)\n", what,
               (unsigned)rt_atomic_load(&s_flash_writes),
               (unsigned)rt_atomic_load(&s_flash_write_bytes),
               (unsigned)rt_atomic_load(&s_flash_erases),
               (unsigned)rt_atomic_load(&s_flash_erase_bytes));
}

/* ----------------------------------------------------------------- helpers -- */

/* Microseconds since boot as a 32-bit millisecond count. A run is short enough
 * that wrapping 32 bits of milliseconds (49 days) cannot happen. */
static rt_uint32_t bench_ms_since(rt_int64_t t0)
{
    return (rt_uint32_t)((esp_timer_get_time() - t0) / 1000);
}

static void bench_fill(rt_uint8_t *buf, rt_uint32_t len, rt_uint8_t seed)
{
    rt_uint32_t i;

    for (i = 0; i < len; i++)
    {
        buf[i] = (rt_uint8_t)(seed + i);
    }
}

/* ------------------------------------------------------------------- tests -- */

static int bench_bulk_write(rt_uint32_t *out_ms, rt_uint32_t *out_kbps)
{
    FILE *f;
    rt_int64_t t0;
    rt_uint32_t ms;
    int c;

    f = fopen(BENCH_FILE, "wb");
    if (f == RT_NULL)
    {
        rt_kprintf("bench: cannot open %s for write: errno=%d (%s)\n",
                   BENCH_FILE, errno, strerror(errno));
        return -1;
    }

    bench_counters_reset();
    t0 = esp_timer_get_time();

    for (c = 0; c < BENCH_CHUNKS; c++)
    {
        size_t n;

        bench_fill(s_bench_buf, sizeof(s_bench_buf), (rt_uint8_t)c);

        n = fwrite(s_bench_buf, 1, sizeof(s_bench_buf), f);
        if (n != sizeof(s_bench_buf))
        {
            rt_kprintf("bench: short write at chunk %d (%u of %u)\n", c,
                       (unsigned)n, (unsigned)sizeof(s_bench_buf));
            fclose(f);
            return -1;
        }
    }

    /*
     * fclose inside the timed region, not after it.
     *
     * f_close() is what flushes the last cluster and writes the directory entry;
     * timing only the fwrite calls would report a number that excludes the work
     * which makes the data durable.
     */
    fclose(f);

    ms = bench_ms_since(t0);
    *out_ms = ms;
    *out_kbps = (ms == 0) ? 0 : (rt_uint32_t)((BENCH_TOTAL / 1024) * 1000 / ms);

    bench_counters_print("bulk write 256 KB");
    return 0;
}

static int bench_bulk_read(rt_uint32_t *out_ms, rt_uint32_t *out_kbps)
{
    FILE *f;
    rt_int64_t t0;
    rt_uint32_t ms;
    rt_uint32_t total = 0;

    f = fopen(BENCH_FILE, "rb");
    if (f == RT_NULL)
    {
        rt_kprintf("bench: cannot open %s for read: errno=%d (%s)\n",
                   BENCH_FILE, errno, strerror(errno));
        return -1;
    }

    t0 = esp_timer_get_time();

    for (;;)
    {
        size_t n = fread(s_bench_buf, 1, sizeof(s_bench_buf), f);

        if (n == 0)
        {
            break;
        }

        total += (rt_uint32_t)n;
    }

    fclose(f);

    ms = bench_ms_since(t0);
    *out_ms = ms;
    *out_kbps = (ms == 0) ? 0 : (rt_uint32_t)((total / 1024) * 1000 / ms);
    return 0;
}

/*
 * Small-file burst: create N files, overwrite one repeatedly, then delete them.
 *
 * This is the pattern that exposes the stack's real cost. Each file costs one
 * directory entry plus one cluster, and on this stack each of those is a 4 KB
 * program preceded by a 4 KB erase - so a 64-byte file spends roughly 21 KB of
 * flash. The overwrite pass isolates "create" from "write to an existing file";
 * the delete pass isolates the cluster-free path.
 */
static int bench_small(rt_uint32_t *out_create_ms, rt_uint32_t *out_overwrite_ms,
                       rt_uint32_t *out_delete_ms)
{
    rt_int64_t t0;
    char path[64];
    rt_uint8_t payload[BENCH_SMALL_LEN];
    int i;

    bench_fill(payload, sizeof(payload), 0x40);

    /* ---- create ---- */
    bench_counters_reset();
    t0 = esp_timer_get_time();

    for (i = 0; i < BENCH_SMALL_N; i++)
    {
        FILE *f;

        rt_snprintf(path, sizeof(path), BENCH_TMP, i);

        f = fopen(path, "wb");
        if (f == RT_NULL)
        {
            rt_kprintf("bench: create %s failed: errno=%d (%s)\n", path, errno,
                       strerror(errno));
            return -1;
        }

        (void)fwrite(payload, 1, sizeof(payload), f);
        fclose(f);
    }

    *out_create_ms = bench_ms_since(t0);
    bench_counters_print("create 32 small files");

    /* ---- overwrite one file 8x ---- */
    bench_counters_reset();
    t0 = esp_timer_get_time();

    for (i = 0; i < 8; i++)
    {
        FILE *f;

        rt_snprintf(path, sizeof(path), BENCH_TMP, 0);

        f = fopen(path, "wb");
        if (f != RT_NULL)
        {
            (void)fwrite(payload, 1, sizeof(payload), f);
            fclose(f);
        }
    }

    *out_overwrite_ms = bench_ms_since(t0);
    bench_counters_print("overwrite one file 8x");

    /* ---- delete ---- */
    t0 = esp_timer_get_time();

    for (i = 0; i < BENCH_SMALL_N; i++)
    {
        rt_snprintf(path, sizeof(path), BENCH_TMP, i);

        if (remove(path) != 0)
        {
            rt_kprintf("bench: remove %s failed: errno=%d (%s)\n", path, errno,
                       strerror(errno));
            return -1;
        }
    }

    *out_delete_ms = bench_ms_since(t0);
    return 0;
}

/* ----------------------------------------------------------------- command -- */

static int s3fsbench_run(int argc, char **argv)
{
    /*
     * Optional selector, so one test can run without the others.
     *
     * Needed in practice: with 512-byte sectors the bulk write took 82 seconds,
     * and measuring anything else on that configuration would otherwise mean
     * waiting for a test whose result was already known to be bad.
     *
     *   s3fsbench          all three
     *   s3fsbench write    bulk write only
     *   s3fsbench read     bulk read only
     *   s3fsbench small    small-file burst only
     */
    const char *only = (argc >= 2) ? argv[1] : RT_NULL;
    rt_bool_t do_write = (only == RT_NULL) || (rt_strcmp(only, "write") == 0);
    rt_bool_t do_read  = (only == RT_NULL) || (rt_strcmp(only, "read") == 0);
    rt_bool_t do_small = (only == RT_NULL) || (rt_strcmp(only, "small") == 0);

    if (only != RT_NULL && !do_write && !do_read && !do_small)
    {
        rt_kprintf("s3fsbench: unknown test \"%s\"; use write, read or small\n",
                   only);
        return -1;
    }

    return s3_bench_run(do_write, do_read, do_small);
}

static int s3_bench_run(rt_bool_t do_write, rt_bool_t do_read, rt_bool_t do_small)
{
    uint64_t total_bytes = 0, free_bytes = 0;
    esp_err_t err;

    rt_kprintf("s3fsbench: FAT on \"%s\", %u-byte sectors, %u-byte clusters\n",
               "storage", (unsigned)CONFIG_WL_SECTOR_SIZE,
               (unsigned)CONFIG_WL_SECTOR_SIZE);

    err = esp_vfs_fat_info(BENCH_MOUNT, &total_bytes, &free_bytes);
    if (err != ESP_OK)
    {
        rt_kprintf("s3fsbench: %s is not mounted (%s); run `s3fs` first\n",
                   BENCH_MOUNT, esp_err_to_name(err));
        return -1;
    }

    rt_kprintf("s3fsbench: volume %u KB total, %u KB free\n",
               (unsigned)(total_bytes / 1024), (unsigned)(free_bytes / 1024));

    /*
     * Say whether this volume has been used before, because the numbers depend
     * on it.
     *
     * Measured on the same image, same partition, one command apart:
     *
     *   freshly formatted : 152 writes / 136 erases / 5374 ms
     *   used for a while   : 190 writes / 170 erases / 6989 ms
     *
     * Over 20% apart, and not drift - the write count is exactly reproducible
     * on each kind of volume (190 twice in a row, 152 twice in a row). A used
     * volume has its directory entries spread out, so creating a file updates
     * more FAT sectors, and every FAT sector write is an erase.
     *
     * Without this line the difference reads as a performance regression. The
     * `free` figure is the tell: a volume that has only ever held the benchmark
     * reports the full size, one that has been formatted reports slightly less.
     */
    rt_kprintf("s3fsbench: measure on a freshly formatted volume - a used one "
               "gives ~25%% more writes and ~30%% more time\n");

    if (do_write)
    {
        rt_uint32_t ms = 0, kbps = 0;

        if (bench_bulk_write(&ms, &kbps) != 0)
        {
            return -1;
        }

        rt_kprintf("s3fsbench: write  %u KB in %u ms -> %u KB/s\n",
                   (unsigned)(BENCH_TOTAL / 1024), (unsigned)ms, (unsigned)kbps);
    }

    if (do_read)
    {
        rt_uint32_t ms = 0, kbps = 0;

        if (bench_bulk_read(&ms, &kbps) != 0)
        {
            return -1;
        }

        rt_kprintf("s3fsbench: read   %u KB in %u ms -> %u KB/s\n",
                   (unsigned)(BENCH_TOTAL / 1024), (unsigned)ms, (unsigned)kbps);
    }

    if (do_small)
    {
        rt_uint32_t c_ms = 0, o_ms = 0, d_ms = 0;

        if (bench_small(&c_ms, &o_ms, &d_ms) != 0)
        {
            return -1;
        }

        rt_kprintf("s3fsbench: create %d x %u B in %u ms (%u files/s, %u ms each)\n",
                   BENCH_SMALL_N, (unsigned)BENCH_SMALL_LEN, (unsigned)c_ms,
                   (c_ms == 0) ? 0 : (unsigned)((BENCH_SMALL_N * 1000) / c_ms),
                   (c_ms == 0) ? 0 : (unsigned)(c_ms / BENCH_SMALL_N));
        rt_kprintf("s3fsbench: overwrite one file 8x in %u ms (%u ms each)\n",
                   (unsigned)o_ms, (unsigned)(o_ms / 8));
        rt_kprintf("s3fsbench: delete %d files in %u ms (%u ms each)\n",
                   BENCH_SMALL_N, (unsigned)d_ms,
                   (unsigned)(d_ms / BENCH_SMALL_N));
    }

    /* The bulk file only exists if the write test ran; removing it otherwise
     * would print a spurious warning. */
    if (!do_write)
    {
        return 0;
    }

    /*
     * Space reclaimed, measured AFTER removing the bulk file.
     *
     * Measured before the remove in the first version, so the delta always
     * contained the 256 KB bench.bin and the number was meaningless.
     */
    if (remove(BENCH_FILE) != 0)
    {
        rt_kprintf("s3fsbench: warning - could not remove %s\n", BENCH_FILE);
    }

    {
        uint64_t t2 = 0, f2 = 0;

        if (esp_vfs_fat_info(BENCH_MOUNT, &t2, &f2) == ESP_OK)
        {
            rt_int32_t reclaimed_kb = (rt_int32_t)(f2 / 1024) -
                                      (rt_int32_t)(free_bytes / 1024);

            rt_kprintf("s3fsbench: free %u KB -> %u KB after removing the bulk "
                       "file (%d KB net)\n",
                       (unsigned)(free_bytes / 1024), (unsigned)(f2 / 1024),
                       (int)reclaimed_kb);
        }
    }

    return 0;
}

/*
 * The benchmark runs long enough to trip the hardware watchdog once both radios
 * are up.
 *
 * Measured: `s3fsbench write` takes about 0.9 s with the radio idle, but with
 * Wi-Fi associated and BLE advertising every flash window has to wait for the
 * IPC callback while the radio tasks compete for the CPU, and the run exceeds
 * the 5 s watchdog timeout - the board resets mid-benchmark with
 * `rst:0x7 (TG0WDT_SYS_RST)` and the numbers are lost.
 *
 * Pausing the feeder for the duration is what `s3fs` already does around mount
 * and format. The benchmark is an instrument, not a service: it is acceptable
 * for it to declare "do not judge liveness during this measurement", and it is
 * not acceptable to lose the measurement.
 *
 * Weak, so a build without s3wdt still links.
 */
void s3_wdt_pause(void) __attribute__((weak));
void s3_wdt_resume(void) __attribute__((weak));

/* Provided by s3wifi.c. See the note below for why the radio has to go down. */
int s3_wifi_suspend_for_flash(void) __attribute__((weak));
int s3_wifi_resume_after_flash(void) __attribute__((weak));

/* Provided by s3ble.c. */
rt_bool_t s3ble_is_up(void) __attribute__((weak));
rt_err_t s3ble_shutdown_quiet(void) __attribute__((weak));

static int s3fsbench_run(int argc, char **argv);

static int cmd_s3fsbench(int argc, char **argv)
{
    int rc;
    int radio_down = 0;
    int ble_down = 0;

    /*
     * Stop the radio for the duration.
     *
     * Measured: `s3fsbench write` finishes in 1045 ms with the radio idle, but
     * with Wi-Fi associated and BLE advertising it has not produced a single
     * number after 200 s. Pausing the watchdog first turned that into a silent
     * hang rather than a reset, which is how it was finally obvious that this is
     * not slowness - it is the same interleaving failure that `s3fs erase` hits
     * (docs/KNOWN-ISSUES.md section 2): the radio writes NVS from its own task,
     * and those writes cannot share the cache-disable window with a long run of
     * flash writes.
     *
     * So the benchmark takes the same medicine as the erase: radio down, measure,
     * radio back up.
     */
    if (s3_wifi_suspend_for_flash != RT_NULL &&
        s3_wifi_suspend_for_flash() == 0)
    {
        radio_down = 1;
        rt_kprintf("s3fsbench: Wi-Fi suspended for the measurement\n");
    }

    /*
     * Stop BLE too, for the same reason and on the same evidence.
     *
     * Suspending only the station was measured and is not enough: with BLE still
     * advertising, `s3fsbench write` still produced no numbers. Both radios write
     * NVS from their own tasks, and the benchmark is 79 writes and 71 erases back
     * to back - far denser than the 24 windows an erase needs.
     */
    if (s3ble_is_up != RT_NULL && s3ble_is_up() &&
        s3ble_shutdown_quiet != RT_NULL && s3ble_shutdown_quiet() == RT_EOK)
    {
        ble_down = 1;
        rt_kprintf("s3fsbench: BLE unloaded for the measurement "
                   "(run `s3ble` again afterwards)\n");
    }

    if (s3_wdt_pause != RT_NULL)
    {
        s3_wdt_pause();
    }

    rc = s3fsbench_run(argc, argv);

    if (s3_wdt_resume != RT_NULL)
    {
        s3_wdt_resume();
    }

    if (radio_down && s3_wifi_resume_after_flash != RT_NULL)
    {
        (void)s3_wifi_resume_after_flash();
    }

    return rc;
}

MSH_CMD_EXPORT_ALIAS(cmd_s3fsbench, s3fsbench,
                     measure FAT write/read/small-file throughput);
