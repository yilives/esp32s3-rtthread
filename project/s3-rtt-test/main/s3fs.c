/*
 * A filesystem for the ESP32-S3 RT-Thread image: FAT on the `storage` partition,
 * through IDF's VFS + FATFS + wear levelling.
 *
 * Why FAT and not littlefs
 * ------------------------
 * littlefs is not in either tree. RT-Thread ships it as an online package
 * (`pkgs --update`), which would add a download step to a build that otherwise
 * needs nothing but the two checkouts; ESP-IDF has no littlefs component at all
 * (`components/` has fatfs, spiffs and vfs - checked). FATFS, by contrast, is
 * already compiled into this image:
 *
 *   libfatfs.a           esp_vfs_fat_spiflash_mount_rw_wl, f_mount, the format helper
 *   libwear_levelling.a  wl_mount, wl_unmount
 *   libvfs.a             esp_vfs_register
 *
 * so mounting it costs one call and no new dependency. FAT's lack of wear
 * levelling is handled: esp_vfs_fat_spiflash_mount_rw_wl() goes through
 * wear_levelling, which spreads writes across the whole partition. FAT also
 * means the partition can be pulled off the chip and read on a PC, which
 * littlefs cannot do without extra tooling.
 *
 * Why not RT-Thread's DFS
 * -----------------------
 * RT_USING_DFS is off in this image and turning it on would mean choosing
 * between dfs_v1 and dfs_v2, wiring a devfs, and re-porting the drivers to the
 * DFS device model. IDF's VFS already is a POSIX-shaped layer over the same
 * thing, and it is the one the Wi-Fi/BLE components expect. Using it keeps the
 * kernel side untouched.
 */

#include <rtthread.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "diskio_wl.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "esp_heap_caps.h"
#include "esp_partition.h"

#define S3FS_PARTITION      "storage"
#define S3FS_MOUNT_POINT    "/storage"

/* One file is enough to prove the mount, so the FAT allocation unit count can
 * stay at the default. max_files 5 leaves room for a log file, a config file and
 * two scratch files plus the mount's own descriptor. */
/*
 * Open-file slots, and with CONFIG_FATFS_PER_FILE_CACHE each slot also carries a
 * sector-sized buffer. Five was the original number - enough for a log, a config,
 * two scratch files and the mount itself - but a larger table costs internal RAM
 * (S3FS_MAX_FILES * 4 KB) and buys nothing unless the application really keeps
 * that many files open. Measured: raising it from 5 to 16 changed nothing,
 * because the benchmark keeps one file open at a time. Left at 8 as a compromise
 * for the field use this image targets; the comment records that the number was
 * measured rather than guessed.
 */
#define S3FS_MAX_FILES      8

static int s3_fs_read_only(void);

/*
 * The hardware watchdog (s3wdt.c) has to stand down around a format or a mount
 * that formats: both erase the whole 1 MB partition, and the flash driver runs
 * the erase inside spi_flash_op_lock() -> rt_enter_critical(), where no thread is
 * scheduled at all - so the feeder cannot possibly run. Measured with a 3 s
 * timeout: the format was cut short, the FAT was half-written, and the next
 * mount failed with EACCES.
 *
 * Weak definitions so this file links without s3wdt.c present.
 */
void s3_wdt_pause(void) __attribute__((weak));

/*
 * Provided by s3wifi.c. Weak so a build without Wi-Fi still links.
 *
 * The radio has to be down while the partition is being erased. Measured: with
 * Wi-Fi associated, `s3fs erase` stops after three flash windows and never
 * returns; with `s3wifi stop` first, the same erase completes in one pass. The
 * stack writes NVS from its own task, so its flash operations interleave with
 * the erase's, and each interleave is a cache-disable window on both cores -
 * one of them eventually loses the handshake.
 */
int s3_wifi_suspend_for_flash(void) __attribute__((weak));
int s3_wifi_resume_after_flash(void) __attribute__((weak));
void s3_wdt_resume(void) __attribute__((weak));

static void fs_wdt_pause(void)
{
    if (s3_wdt_pause != RT_NULL)
    {
        s3_wdt_pause();
    }
}

static void fs_wdt_resume(void)
{
    if (s3_wdt_resume != RT_NULL)
    {
        s3_wdt_resume();
    }
}

static wl_handle_t s_fs_wl = WL_INVALID_HANDLE;
static rt_bool_t   s_fs_mounted;

/* Has any mount succeeded in this boot? Gates automatic formatting. */
static rt_bool_t   s_fs_mounted_once;



/*
 * Mount, formatting on the first use of a blank partition.
 *
 * The format has to be conditional, not unconditional: reformatting on every
 * boot would erase whatever the application stored. `format_if_mount_failed`
 * in the mount config does exactly this - it tries the mount, and only if that
 * fails does it format - which is why it is set here rather than calling
 * esp_vfs_fat_spiflash_format_rw_wl() up front.
 */
/*
 * Have we ever had a volume in this boot?
 *
 * This is the only sound input to the "may I format?" decision, and reaching it
 * took two wrong attempts worth recording:
 *
 *   1. Peeking for the 0x55AA boot signature at physical offset 510 is WRONG.
 *      That offset is inside the partition, so it is a wear-levelling block
 *      header, not logical sector 0 - the FAT boot sector lives at whatever
 *      physical block wear_levelling mapped it to. Measured: after wiping the
 *      first 8 KB with esptool the volume still mounted and worked, so the check
 *      was looking at the wrong bytes and answering "no filesystem" on a healthy
 *      partition.
 *
 *   2. Probing the logical sector through esp_vfs_fat would require mounting
 *      first, which is the thing that is failing.
 *
 * So the decision is stateful instead: the first mount of a boot is allowed to
 * format (a factory-fresh partition must be formattable, and at boot there is
 * nothing running that could have produced data yet in this boot, though the data
 * on flash may well be from a previous one). Once a mount has succeeded, a later
 * failure means the volume broke while the system was running - an interrupted
 * format, a torn FAT - and reformatting would destroy exactly the evidence and
 * the data that matter. That case fails loudly and points at `s3fs erase`.
 */
static int s3_fs_mount(void)
{
    esp_vfs_fat_mount_config_t cfg = {
        /*
         * Format only the first mount of a boot; see the note above on why this is
         * stateful rather than a probe of the flash contents.
         *
         * The cost of getting this wrong in the permissive direction is silent
         * data loss, and it is not hypothetical: a format interrupted by a
         * watchdog reset left a volume that mounted but failed every fopen with
         * EACCES, and the default `format_if_mount_failed = true` would have
         * erased it on the next boot without a word.
         */
        .format_if_mount_failed = !s_fs_mounted_once,
        .max_files = S3FS_MAX_FILES,
        /*
         * One cluster per wear-levelling sector (4 KB).
         *
         * 8 KB clusters were measured and rejected: 5316 ms versus 5297 ms for the
         * 32-small-file burst, i.e. no gain, because the cost is one sector
         * program per file either way and the FAT is short enough at 256 clusters
         * that halving it does not save a write. Larger clusters would only add
         * internal fragmentation.
         */
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
        .disk_status_check_enable = false,
        /*
         * One FAT copy instead of two.
         *
         * Two copies exist so a damaged primary can be replaced by the mirror.
         * That protection is worth something on a raw block device and close to
         * nothing here: every write already goes through wear_levelling, which
         * itself keeps a spare block and a position-update record, so the failure
         * mode "one FAT copy torn, the other intact" is far rarer than it is on
         * bare flash. What the second copy definitely costs is a full extra 4 KB
         * program per metadata update - measured at 37.6 sector writes per
         * fclose() versus 18.1 with a single copy.
         *
         * The trade is real, not free: with one copy, a torn FAT write leaves the
         * volume unmountable instead of repairable. `s3fs erase` recovers that by
         * wiping the partition, and this partition holds application data, not
         * firmware - a reflash is not required.
         */
        .use_one_fat = true,
    };
    esp_err_t err;

    if (s_fs_mounted)
    {
        rt_kprintf("s3fs: already mounted at %s\n", S3FS_MOUNT_POINT);
        return RT_EOK;
    }

    fs_wdt_pause();
    err = esp_vfs_fat_spiflash_mount_rw_wl(S3FS_MOUNT_POINT, S3FS_PARTITION,
                                           &cfg, &s_fs_wl);
    fs_wdt_resume();
    if (err != ESP_OK)
    {
        rt_kprintf("s3fs: mount failed 0x%x (%s)\n", err, esp_err_to_name(err));
        rt_kprintf("s3fs: is the partition table flashed? `storage` starts at "
                   "0x310000 and is 1 MB\n");
        return -RT_ERROR;
    }

    s_fs_mounted = RT_TRUE;
    s_fs_mounted_once = RT_TRUE;
    return RT_EOK;
}

static int s3_fs_unmount(void)
{
    esp_err_t err;

    if (!s_fs_mounted)
    {
        rt_kprintf("s3fs: not mounted\n");
        return RT_EOK;
    }

    err = esp_vfs_fat_spiflash_unmount_rw_wl(S3FS_MOUNT_POINT, s_fs_wl);
    if (err != ESP_OK)
    {
        rt_kprintf("s3fs: unmount failed 0x%x (%s)\n", err, esp_err_to_name(err));
        return -RT_ERROR;
    }

    s_fs_wl = WL_INVALID_HANDLE;
    s_fs_mounted = RT_FALSE;
    return RT_EOK;
}

/*
 * `s3fs` - mount, then read/write/list/remove one file.
 *
 * The self-test is deliberately a full round trip rather than "mount returned
 * ESP_OK": a mount can succeed against a partition whose FAT is unusable, and
 * the only way to know otherwise is to write something and read it back. The
 * payload is longer than one sector so it also proves the wear-levelling layer
 * maps more than a single block.
 */
static int cmd_s3fs(int argc, char **argv)
{
    char path[64];
    char buf[512];
    char expect[512];
    FILE *f;
    size_t n;
    int i;
    esp_err_t err;

    if (argc >= 2 && rt_strcmp(argv[1], "unmount") == 0)
    {
        return s3_fs_unmount() == RT_EOK ? 0 : -1;
    }

    if (argc >= 2 && rt_strcmp(argv[1], "read") == 0)
    {
        return s3_fs_read_only() == RT_EOK ? 0 : -1;
    }

    /*
     * `s3fs erase` - wipe the whole partition, not just the FAT structures.
     *
     * Needed because `format` leaves the partition in an inconsistent state if it
     * is interrupted (a watchdog reset mid-erase does exactly that), and a
     * half-written FAT reports FR_DENIED / EACCES on the next open rather than
     * "no filesystem". Erasing first makes the next mount's format run against
     * clean flash.
     */
    if (argc >= 2 && rt_strcmp(argv[1], "erase") == 0)
    {
        const esp_partition_t *part;
        esp_err_t e;

        if (s_fs_mounted)
        {
            rt_kprintf("s3fs: unmount before erasing\n");
            return -1;
        }

        part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                        ESP_PARTITION_SUBTYPE_ANY,
                                        S3FS_PARTITION);
        if (part == RT_NULL)
        {
            rt_kprintf("s3fs: partition \"%s\" not found\n", S3FS_PARTITION);
            return -1;
        }

        rt_kprintf("s3fs: erasing %u bytes at 0x%x ...\n",
                   (unsigned)part->size, (unsigned)part->address);

        /* Radio down first: an associated station writes NVS from its own task,
         * and those writes cannot share the flash window with a 1 MB erase. */
        if (s3_wifi_suspend_for_flash != RT_NULL &&
            s3_wifi_suspend_for_flash() == 0)
        {
            rt_kprintf("s3fs: Wi-Fi suspended for the erase\n");
        }

        fs_wdt_pause();
        e = esp_partition_erase_range(part, 0, part->size);
        fs_wdt_resume();

        if (s3_wifi_resume_after_flash != RT_NULL)
        {
            (void)s3_wifi_resume_after_flash();
        }
        if (e != ESP_OK)
        {
            rt_kprintf("s3fs: erase failed 0x%x\n", e);
            return -1;
        }

        /*
         * The partition is now blank, so the next mount MUST be allowed to
         * format it.
         *
         * s_fs_mounted_once is what makes format_if_mount_failed false after the
         * first successful mount - a deliberate guard so a mount failure does
         * not silently wipe user data (see the note at s3_fs_mount). But erase
         * has just removed the filesystem on purpose, and leaving the flag set
         * made every later `s3fs` fail:
         *
         *   E vfs_fat_spiflash: f_mount failed with error:
         *       "No filesystem detected" [13]
         *   s3fs: mount failed 0xffffffff (ESP_FAIL)
         *
         * Measured: after `s3fs unmount` + `s3fs erase`, both the first and the
         * second `s3fs` failed, so "next `s3fs` will format and mount" - the
         * line this command prints - was untrue. Only a reboot recovered,
         * because that resets the flag.
         */
        s_fs_mounted_once = RT_FALSE;

        rt_kprintf("s3fs: erased; next `s3fs` will format and mount\n");
        return 0;
    }

    if (argc >= 2 && rt_strcmp(argv[1], "format") == 0)
    {
        if (s_fs_mounted)
        {
            rt_kprintf("s3fs: unmount before formatting\n");
            return -1;
        }

        /*
         * Radio down first, same as `erase` above.
         *
         * Measured: formatting with Wi-Fi associated happened to succeed 3/3,
         * so this is not fixing an observed failure. It is closing the same
         * window `erase` closed: the station writes NVS from its own task, and
         * those writes cannot share the cache-disable window with a long run of
         * flash operations. `erase` used to fail silently at the fourth window;
         * formatting does less work and currently gets away with it, which is
         * not the same as being safe.
         *
         * The asymmetry was the real defect: two commands doing the same kind
         * of thing, one guarded and one not, with no note explaining why.
         */
        if (s3_wifi_suspend_for_flash != RT_NULL &&
            s3_wifi_suspend_for_flash() == 0)
        {
            rt_kprintf("s3fs: Wi-Fi suspended for the format\n");
        }

        fs_wdt_pause();
        err = esp_vfs_fat_spiflash_format_rw_wl(S3FS_MOUNT_POINT,
                                                S3FS_PARTITION);
        fs_wdt_resume();

        if (s3_wifi_resume_after_flash != RT_NULL)
        {
            (void)s3_wifi_resume_after_flash();
        }
        if (err != ESP_OK)
        {
            rt_kprintf("s3fs: format failed 0x%x (%s)\n", err,
                       esp_err_to_name(err));
            return -1;
        }

        rt_kprintf("s3fs: formatted %s\n", S3FS_PARTITION);
        return 0;
    }

    if (s3_fs_mount() != RT_EOK)
    {
        return -1;
    }

    rt_snprintf(path, sizeof(path), "%s/s3fs.txt", S3FS_MOUNT_POINT);

    /*
     * Build the expected payload: a repeating printable pattern, so a corrupt
     * read is obvious in the dump rather than looking like a plausible string.
     */
    for (i = 0; i < (int)sizeof(expect) - 1; i++)
    {
        expect[i] = (char)('A' + (i % 26));
    }
    expect[sizeof(expect) - 1] = '\0';

    f = fopen(path, "w");
    if (f == RT_NULL)
    {
        rt_kprintf("s3fs: FAIL, cannot open %s for write: errno=%d (%s)\n",
                   path, errno, strerror(errno));
        {
            /* rt_memory_info takes three non-NULL pointers. `free`'s own numbers
             * come from here too, so these match what the shell reports. */
            rt_size_t heap_total = 0;
            rt_size_t heap_used = 0;
            rt_size_t heap_maxused = 0;

            rt_memory_info(&heap_total, &heap_used, &heap_maxused);
            rt_kprintf("s3fs: rt heap total=%u used=%u; idf free=%u largest=%u\n",
                       (unsigned)heap_total, (unsigned)heap_used,
                       (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                       (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        }
        return -1;
    }

    n = fwrite(expect, 1, sizeof(expect) - 1, f);
    fclose(f);

    if (n != sizeof(expect) - 1)
    {
        rt_kprintf("s3fs: FAIL, short write (%u of %u)\n",
                   (unsigned)n, (unsigned)(sizeof(expect) - 1));
        return -1;
    }

    /* Reopen rather than rewind: that is what proves the data reached the
     * filesystem, not just a buffer in the stdio layer. */
    f = fopen(path, "r");
    if (f == RT_NULL)
    {
        rt_kprintf("s3fs: FAIL, cannot open %s for read\n", path);
        return -1;
    }

    rt_memset(buf, 0, sizeof(buf));
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);

    if (n != sizeof(expect) - 1 || rt_strcmp(buf, expect) != 0)
    {
        rt_kprintf("s3fs: FAIL, read back %u bytes, content differs\n",
                   (unsigned)n);
        return -1;
    }

    rt_kprintf("s3fs: PASS, mounted at %s, wrote and read back %u bytes\n",
               S3FS_MOUNT_POINT, (unsigned)n);

    /* Leave the file in place: a later boot then proves the data survived a
     * reset, which a write/read in the same session cannot show. */
    rt_kprintf("s3fs: %s left in place; reboot and rerun to prove persistence\n",
               path);
    return 0;
}

/*
 * `s3fs read` - read the file back WITHOUT writing it first.
 *
 * This is what actually proves persistence. `s3fs` on its own writes and then
 * reads, so it would pass against a freshly formatted partition every boot;
 * only a read with no preceding write can distinguish "the filesystem works"
 * from "the data survived". The expected content is whatever the write path
 * above produces, so a mismatch means the bytes changed on flash.
 */
static int s3_fs_read_only(void)
{
    char path[64];
    char buf[512];
    char expect[512];
    FILE *f;
    size_t n;
    int i;

    if (s3_fs_mount() != RT_EOK)
    {
        return -1;
    }

    rt_snprintf(path, sizeof(path), "%s/s3fs.txt", S3FS_MOUNT_POINT);

    f = fopen(path, "r");
    if (f == RT_NULL)
    {
        rt_kprintf("s3fs read: FAIL, %s not found - run `s3fs` first\n", path);
        return -1;
    }

    rt_memset(buf, 0, sizeof(buf));
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);

    for (i = 0; i < (int)sizeof(expect) - 1; i++)
    {
        expect[i] = (char)('A' + (i % 26));
    }
    expect[sizeof(expect) - 1] = '\0';

    if (n != sizeof(expect) - 1 || rt_strcmp(buf, expect) != 0)
    {
        rt_kprintf("s3fs read: FAIL, %u bytes, content differs from what `s3fs` "
                   "writes\n", (unsigned)n);
        return -1;
    }

    rt_kprintf("s3fs read: PASS, %u bytes intact after reboot\n", (unsigned)n);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_s3fs, s3fs, mount FAT on the storage partition and round-trip a file);

/*
 * Mount at boot, before Wi-Fi and Bluetooth exist.
 *
 * This is not a convenience - it is the only ordering that is safe today. Both
 * wireless stacks write NVS from their own tasks, and a flash operation cannot
 * share the flash controller with them: measured, formatting the partition while
 * Wi-Fi and Bluetooth were already running either deadlocked the two cores in the
 * cache-off handshake or left the FAT half-written, so the next fopen returned
 * EACCES. Formatting first and then starting the wireless stacks works, and that
 * is exactly what boot-time mounting gives.
 *
 * Deliberately INIT_APP_EXPORT and not INIT_BOARD_EXPORT: the VFS and FATFS need
 * the RT-Thread scheduler (their locks are mutexes), which does not exist at the
 * board stage.
 *
 * Failure is not fatal. A board with no `storage` partition, or with an erased
 * one, still boots and still runs every other command; `s3fs` reports why.
 */
static int s3_fs_boot_mount(void)
{
    if (s3_fs_mount() == RT_EOK)
    {
        rt_kprintf("s3fs: %s mounted at boot\n", S3FS_PARTITION);
    }
    else
    {
        rt_kprintf("s3fs: boot mount failed; run `s3fs erase` then `s3fs`\n");
    }

    return RT_EOK;
}
INIT_APP_EXPORT(s3_fs_boot_mount);
