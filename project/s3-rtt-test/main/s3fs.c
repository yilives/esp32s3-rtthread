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
/* opendir / readdir, from the IDF VFS (components/vfs/linux_include). */
#include <dirent.h>

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

/* How much `s3fs cat` will print before truncating. */
#define S3FS_CAT_LIMIT      (1024)

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
/*
 * One definition of the mkfs/mount parameters, used by both the mount and the
 * explicit `s3fs format`.
 *
 * It has to be shared. `s3fs format` originally called
 * esp_vfs_fat_spiflash_format_rw_wl(), which formats with IDF's defaults
 * (two FAT copies) while the mount config asks for one - so the same command
 * produced volumes with different write amplification depending on which path
 * created them. Measured 152 writes / 5506 ms with one FAT and 190 writes /
 * 7249 ms with two.
 */
static esp_vfs_fat_mount_config_t s3_fat_cfg = {
    /*
     * format_if_mount_failed is set per call, not here - see s3_fs_mount().
     * The cost of getting it wrong in the permissive direction is silent data
     * loss: a format interrupted by a watchdog reset left a volume that mounted
     * but failed every fopen with EACCES, and `format_if_mount_failed = true`
     * would have erased it on the next boot without a word.
     */
    .format_if_mount_failed = false,
    .max_files = S3FS_MAX_FILES,
    /*
     * One cluster per wear-levelling sector (4 KB).
     *
     * 8 KB clusters were measured and rejected: no gain, because the cost is one
     * sector program per file either way and the FAT is short enough at 256
     * clusters that halving it does not save a write. Larger clusters would only
     * add internal fragmentation.
     */
    .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    .disk_status_check_enable = false,
    /*
     * One FAT copy instead of two.
     *
     * Two copies exist so a damaged primary can be replaced by the mirror. That
     * protection is worth something on a raw block device and close to nothing
     * here: every write already goes through wear_levelling, which itself keeps a
     * spare block and a position-update record, so "one FAT copy torn, the other
     * intact" is far rarer than on bare flash. What the second copy definitely
     * costs is a full extra 4 KB program per metadata update - measured 190
     * writes instead of 152 for the 32-small-file benchmark, 7249 ms instead of
     * 5506.
     *
     * The trade is real, not free: with one copy, a torn FAT write leaves the
     * volume unmountable instead of repairable. `s3fs erase` recovers that by
     * wiping the partition, and this partition holds application data, not
     * firmware - a reflash is not required.
     */
    .use_one_fat = true,
};

static int s3_fs_mount(void)
{
    esp_err_t err;

    if (s_fs_mounted)
    {
        rt_kprintf("s3fs: already mounted at %s\n", S3FS_MOUNT_POINT);
        return RT_EOK;
    }

    /* format_if_mount_failed is the one field that depends on boot state, so
     * the shared config is copied rather than passed directly. */
    esp_vfs_fat_mount_config_t mount_cfg = s3_fat_cfg;
    mount_cfg.format_if_mount_failed = !s_fs_mounted_once;

    fs_wdt_pause();
    err = esp_vfs_fat_spiflash_mount_rw_wl(S3FS_MOUNT_POINT, S3FS_PARTITION,
                                           &mount_cfg, &s_fs_wl);
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

/*
 * Take the radio down around a long flash operation.
 *
 * Called by both `s3fs erase` and `s3fs format`, which have to do the same
 * thing for the same reason: an associated station writes NVS from its own
 * task, and those writes cannot share the cache-disable window with a partition
 * erase. The two call sites used to open-code this, which is the shape that
 * produced the s3ble deinit bug - the same logic twice, a later fix applied to
 * one copy.
 *
 * Returns 0 if the radio was actually suspended, non-zero if there was nothing
 * to suspend or the stop failed. The caller only uses it to decide whether to
 * print; resume is unconditional and checks its own state.
 */
static int s3_fs_radio_down(const char *what)
{
    if (s3_wifi_suspend_for_flash != RT_NULL &&
        s3_wifi_suspend_for_flash() == 0)
    {
        rt_kprintf("s3fs: Wi-Fi suspended for the %s\n", what);
        return 0;
    }

    return -1;
}

static void s3_fs_radio_up(void)
{
    if (s3_wifi_resume_after_flash != RT_NULL)
    {
        (void)s3_wifi_resume_after_flash();
    }
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

    /*
     * Say so. This path used to return silently, so a successful unmount printed
     * nothing but the shell prompt - indistinguishable from a command that was
     * never received. Every other state-changing s3fs subcommand reports its
     * result: `format`, `erase`, `rm`. Found by running `s3fs unmount` twice: the
     * first call was silent, the second said "not mounted".
     */
    rt_kprintf("s3fs: unmounted %s\n", S3FS_MOUNT_POINT);
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
/*
 * The expected file contents: a repeating printable pattern.
 *
 * Both `s3fs` (write then verify) and `s3fs read` (verify after reboot) need
 * this, and both used to build it inline. A pattern that repeats is deliberate:
 * a corrupt read shows up as an obviously wrong character rather than as a
 * plausible-looking string.
 *
 * Shared so the two cannot drift - if the pattern changes, `read` must still be
 * verifying what the write produced.
 */
static void s3_fs_build_expect(char *expect, rt_size_t n)
{
    rt_size_t i;

    for (i = 0; i + 1 < n; i++)
    {
        expect[i] = (char)('A' + (i % 26));
    }
    expect[n - 1] = '\0';
}

/*
 * `s3fs ls` - list the volume root.
 *
 * The volume used to be checked only through the one file this command writes,
 * so nothing could show what else was on it. Listing needs opendir/readdir,
 * which come from the IDF VFS rather than from RT-Thread's DFS - DFS is off in
 * this port, and the VFS layer is what backs fopen() here anyway.
 *
 * The size comes from fopen + fseek(END), not from stat(): the VFS does not
 * expose a POSIX stat, and adding a FatFs-level call would mean reaching past
 * the layer this file already uses.
 */
static int s3_fs_ls(void)
{
    DIR *dir;
    struct dirent *ent;
    int count = 0;

    dir = opendir(S3FS_MOUNT_POINT);
    if (dir == RT_NULL)
    {
        rt_kprintf("s3fs ls: cannot open %s (errno=%d)\n", S3FS_MOUNT_POINT, errno);
        return -1;
    }

    rt_kprintf("s3fs ls: %s\n", S3FS_MOUNT_POINT);

    while ((ent = readdir(dir)) != RT_NULL)
    {
        char path[96];
        FILE *f;
        long size = -1;

        rt_snprintf(path, sizeof(path), "%s/%s", S3FS_MOUNT_POINT, ent->d_name);

        f = fopen(path, "rb");
        if (f != RT_NULL)
        {
            if (fseek(f, 0, SEEK_END) == 0)
            {
                size = ftell(f);
            }
            fclose(f);
        }

        if (size >= 0)
        {
            rt_kprintf("        %8ld  %s\n", size, ent->d_name);
        }
        else
        {
            /* A directory, or something ftell could not answer for. */
            rt_kprintf("        %8s  %s/\n", "<dir>", ent->d_name);
        }
        count++;
    }

    closedir(dir);
    rt_kprintf("s3fs ls: %d entry(ies)\n", count);
    return 0;
}

/* `s3fs cat <name>` - print a file's bytes, bounded so it cannot flood the shell. */
static int s3_fs_cat(const char *name)
{
    char path[96];
    char buf[129];
    size_t total = 0;
    FILE *f;

    if (name == RT_NULL || name[0] == '\0')
    {
        rt_kprintf("usage: s3fs cat <name>\n");
        return -1;
    }

    /* Accept both "s3fs.txt" and "/storage/s3fs.txt". */
    if (name[0] == '/')
    {
        rt_snprintf(path, sizeof(path), "%s", name);
    }
    else
    {
        rt_snprintf(path, sizeof(path), "%s/%s", S3FS_MOUNT_POINT, name);
    }

    f = fopen(path, "rb");
    if (f == RT_NULL)
    {
        rt_kprintf("s3fs cat: cannot open %s (errno=%d)\n", path, errno);
        return -1;
    }

    /*
     * Bounded on purpose: this prints to a 115200 baud console, and an unbounded
     * dump of a 256 KB file is a minute of scrolling that hides the command
     * prompt. The truncation is reported rather than silent.
     */
    for (;;)
    {
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);

        if (n == 0)
        {
            break;
        }
        buf[n] = '\0';
        rt_kprintf("%s", buf);
        total += n;

        if (total >= S3FS_CAT_LIMIT)
        {
            rt_kprintf("\n[truncated at %u bytes]\n", (unsigned)total);
            break;
        }
    }
    fclose(f);

    if (total == 0)
    {
        rt_kprintf("s3fs cat: %s is empty\n", path);
    }
    else if (total < S3FS_CAT_LIMIT)
    {
        rt_kprintf("\n[%u bytes]\n", (unsigned)total);
    }

    return 0;
}

/* `s3fs rm <name>` - delete one file. */
static int s3_fs_rm(const char *name)
{
    char path[96];

    if (name == RT_NULL || name[0] == '\0')
    {
        rt_kprintf("usage: s3fs rm <name>\n");
        return -1;
    }

    if (name[0] == '/')
    {
        rt_snprintf(path, sizeof(path), "%s", name);
    }
    else
    {
        rt_snprintf(path, sizeof(path), "%s/%s", S3FS_MOUNT_POINT, name);
    }

    if (remove(path) != 0)
    {
        rt_kprintf("s3fs rm: cannot remove %s (errno=%d)\n", path, errno);
        return -1;
    }

    rt_kprintf("s3fs rm: removed %s\n", path);
    return 0;
}

static int cmd_s3fs(int argc, char **argv)
{
    char path[64];
    char buf[512];
    char expect[512];
    FILE *f;
    size_t n;
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
    if (argc >= 2 && rt_strcmp(argv[1], "ls") == 0)
    {
        if (s3_fs_mount() != RT_EOK)
        {
            return -1;
        }
        return s3_fs_ls() == 0 ? 0 : -1;
    }

    if (argc >= 2 && rt_strcmp(argv[1], "cat") == 0)
    {
        if (s3_fs_mount() != RT_EOK)
        {
            return -1;
        }
        return s3_fs_cat(argc >= 3 ? argv[2] : RT_NULL) == 0 ? 0 : -1;
    }

    if (argc >= 2 && rt_strcmp(argv[1], "rm") == 0)
    {
        if (s3_fs_mount() != RT_EOK)
        {
            return -1;
        }
        return s3_fs_rm(argc >= 3 ? argv[2] : RT_NULL) == 0 ? 0 : -1;
    }

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

        (void)s3_fs_radio_down("erase");

        fs_wdt_pause();
        e = esp_partition_erase_range(part, 0, part->size);
        fs_wdt_resume();

        s3_fs_radio_up();
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
        (void)s3_fs_radio_down("format");

        /*
         * Format with OUR mkfs parameters, not IDF's defaults.
         *
         * esp_vfs_fat_spiflash_format_rw_wl() passes NULL down to
         * esp_vfs_fat_spiflash_format_cfg_rw_wl(), which substitutes
         *
         *   { .max_files = 1, .format_if_mount_failed = true }
         *
         * and formats with the default FAT layout - two FAT copies. Every other
         * way this command creates a volume goes through s3_fs_mount()'s config,
         * which sets use_one_fat. So `s3fs erase` followed by `s3fs` produced a
         * one-FAT volume and `s3fs format` produced a two-FAT one, and nothing
         * said so:
         *
         *   erase -> s3fs (auto-format) -> s3fsbench small : 152 writes
         *   erase -> s3fs format -> s3fs -> s3fsbench small : 190 writes
         *
         * 20% more writes and 24% more time on every metadata update, for a
         * volume that looks identical from `s3fs`. The _cfg_ variant exists for
         * exactly this; passing our config makes the two paths agree.
         */
        /*
         * format_if_mount_failed must be true for this call, or nothing happens.
         *
         * esp_vfs_fat_spiflash_format_cfg_rw_wl() does not format directly: it
         * mounts first and only formats if that mount fails. Passing the shared
         * config unchanged - where format_if_mount_failed is false, because the
         * mount path sets that per boot - made it fail with
         *
         *   E vfs_fat_spiflash: f_mount failed with error: "No filesystem
         *     detected" [13]
         *   E vfs_fat_spiflash: ... Failed to mount
         *
         * and `s3fs format` reported ESP_FAIL. The other fields (allocation unit,
         * use_one_fat) are what this command needs to share; this one is what it
         * needs to override.
         */
        esp_vfs_fat_mount_config_t fmt_cfg = s3_fat_cfg;
        fmt_cfg.format_if_mount_failed = true;

        fs_wdt_pause();
        err = esp_vfs_fat_spiflash_format_cfg_rw_wl(S3FS_MOUNT_POINT,
                                                   S3FS_PARTITION,
                                                   &fmt_cfg);
        fs_wdt_resume();

        s3_fs_radio_up();
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
    s3_fs_build_expect(expect, sizeof(expect));

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

    /*
     * Anything else is a typo, and the default below is not harmless: `s3fs`
     * with an unrecognised subcommand still mounts, writes a file and reads it
     * back. `s3fs bogus` therefore looked like it worked, and did work - just not
     * the thing that was asked for. Same shape in s3ble (starts the radio) and
     * s3rt (prints status). Say so instead.
     */
    if (argc >= 2)
    {
        rt_kprintf("s3fs: unknown subcommand '%s'\n", argv[1]);
        rt_kprintf("usage: s3fs [ls|cat <name>|rm <name>|read|unmount|erase|format]\n");
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

    s3_fs_build_expect(expect, sizeof(expect));

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
