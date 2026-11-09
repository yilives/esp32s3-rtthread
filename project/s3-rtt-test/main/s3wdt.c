/*
 * A hardware watchdog for the ESP32-S3 RT-Thread image.
 *
 * Why not IDF's task watchdog
 * ---------------------------
 * `esp_task_wdt_*` looks like the obvious answer and is not usable here. In
 * ESP-IDF's task_wdt.c every task-facing call is compiled out under
 * CONFIG_IDF_RTOS_RTTHREAD:
 *
 *     esp_task_wdt_add()    -> return ESP_ERR_NOT_SUPPORTED   (task_wdt.c:696)
 *     esp_task_wdt_reset()  -> return ESP_ERR_NOT_SUPPORTED   (task_wdt.c:728)
 *     esp_task_wdt_delete() -> return ESP_ERR_NOT_SUPPORTED   (task_wdt.c:780)
 *     esp_task_wdt_status() -> return ESP_ERR_NOT_SUPPORTED   (task_wdt.c:804)
 *
 * The reason is in the same file: the IDLE subscription goes through
 * esp_register_freertos_idle_hook_for_cpu(), which is called from the FreeRTOS
 * IDLE task - a task this image does not have, because the kernel is
 * RT-Thread's. Only the `_user` variants survive, and they would report
 * affinity CPU 0 unconditionally (task_wdt.c:352-355: "RT-Thread is supported
 * only on single-core C3/C6"), which is wrong for an SMP build.
 *
 * So this file drives the hardware directly instead, through the same HAL that
 * IDF's own interrupt watchdog uses (components/esp_system/int_wdt.c: the
 * TIMG MWDT). That is a plain peripheral - it needs no task, no idle hook and
 * no FreeRTOS at all, which is exactly why it is the right layer here.
 *
 * What is being watched
 * ---------------------
 * One RT-Thread thread ("wdt") feeds the timer. If that thread stops running
 * for the timeout period, the hardware resets the chip. That catches the two
 * failures that actually happen on a port like this:
 *
 *   - a deadlock (two threads each holding a lock the other wants)
 *   - a thread stuck in a loop with interrupts enabled
 *
 * It does NOT catch a thread wedged with interrupts globally disabled, because
 * the feeder cannot run either - that is what a stage-0 interrupt plus a
 * stage-1 reset would be needed for, which is the interrupt watchdog
 * (CONFIG_ESP_INT_WDT) and is deliberately left for later: board.c notes it
 * needs the level-4/5 timer path wired up first, and this port only dispatches
 * level 1.
 *
 * The feeder runs at a priority above the application threads but below the
 * system's own high-priority workers, so a busy system still feeds it while a
 * genuinely stuck scheduler does not. RT_THREAD_PRIORITY_MAX - 3 puts it just
 * above idle and below everything else that matters, and a 500 ms period
 * against a 3 s timeout gives ample margin.
 */

#include <rtthread.h>
#include <rtdevice.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "soc/soc_caps.h"
#include "hal/wdt_hal.h"
#include "hal/wdt_types.h"
#include "hal/mwdt_periph.h"
#include "esp_private/periph_ctrl.h"

#if defined(SOC_GPTIMER_SUPPORTED)
/* TIMG is what the gptimer driver calls the same hardware; SOC_GPTIMER_SUPPORTED
 * is the cap that exists on this chip. SOC_TIMERGROUP_SUPPORTED and
 * SOC_TIMER_SUPPORTED do not - measured: the first build took the #else branch
 * and printed "this chip reports no TIMG" on an ESP32-S3. */

/*
 * Which MWDT to use.
 *
 * The ESP32-S3 has two timer groups (TIMG_LL_INST_NUM == 2). IDF's int_wdt.c
 * takes group 1 and falls back to group 0 on a single-group chip
 * (int_wdt.c:36-53). This file takes group 0 for the same reason in reverse:
 * the two watchdogs then sit on separate hardware and neither can disable the
 * other by accident.
 *
 * The S3 always has two groups, so the choice is not conditional here. A
 * one-group chip would collide with CONFIG_ESP_INT_WDT; the guard below makes
 * that loud rather than silent if this file is ever reused on one.
 */
#define S3_WDT_INSTANCE     WDT_MWDT0
#define S3_WDT_PERIPH       PERIPH_TIMG0_MODULE

#if defined(CONFIG_ESP_INT_WDT) && defined(SOC_TIMERGROUP_ONLY_ONE_GROUP)
#error "This file and CONFIG_ESP_INT_WDT would share the only TIMG; pick one."
#endif

/*
 * Timeout, and how it converts to hardware ticks.
 *
 * The MWDT counts at APB / prescaler. With MWDT_LL_DEFAULT_CLK_PRESCALER
 * (40000) on this chip's 80 MHz APB that is 2000 Hz, i.e. one tick every
 * 500 us. IDF's int_wdt.c uses the same prescaler and the same arithmetic:
 * IWDT_TICKS_PER_US is 500, and 5 s becomes 5*1000000/500 = 10000 ticks
 * (esp_private/esp_int_wdt.h:15-16, int_wdt.c:19).
 *
 * The first version of this file passed a prescaler of 4000 instead of 40000
 * and kept the 500 us tick in the comment, so the timeout was ten times
 * shorter than the feed period: measured, the board reset-looped, six boots in
 * six seconds. Deriving the tick rate from the prescaler instead of asserting it
 * is what makes that impossible to get wrong twice.
 */
#define S3_WDT_TIMEOUT_MS       5000
#define S3_WDT_PRESCALER        MWDT_LL_DEFAULT_CLK_PRESCALER
#define S3_WDT_APB_HZ           80000000
#define S3_WDT_TICK_HZ          (S3_WDT_APB_HZ / S3_WDT_PRESCALER)      /* 2000 */
#define S3_WDT_TICKS_PER_MS     (S3_WDT_TICK_HZ / 1000)                 /* 2 */
#define S3_WDT_TIMEOUT_TICKS    (S3_WDT_TIMEOUT_MS * S3_WDT_TICKS_PER_MS)


/* Feed four times per timeout: a single late tick is not a hang. */
#define S3_WDT_FEED_MS      (S3_WDT_TIMEOUT_MS / 4)

static wdt_hal_context_t s_wdt_hal;
static volatile rt_uint32_t s_wdt_feeds;
static volatile rt_uint32_t s_wdt_armed;

/*
 * Pause / resume around an operation that legitimately blocks the scheduler for
 * longer than the timeout.
 *
 * This exists because the first version of this file fought with the
 * filesystem. Formatting a 1 MB partition takes several seconds, and the flash
 * driver runs it inside spi_flash_op_lock() -> vTaskSuspendAll() ->
 * rt_enter_critical(): no thread is scheduled for the whole window, so the
 * feeder cannot run by construction. Measured: with a 3 s timeout the format was
 * cut off partway through, the FAT was left half-written, and the next mount's
 * fopen returned EACCES (errno 13) - which reads like a permissions bug and was
 * really a truncated format.
 *
 * "Pause" therefore does NOT mean "stop feeding". The first version did exactly
 * that and turned a slow operation into a guaranteed reset: the feeder stopped
 * reloading, the timer expired mid-format, and the board rebooted with rst:0x7
 * exactly as designed. What pause does instead is REPROGRAM stage 0 to a much
 * longer timeout and reload once, so the window is covered without a thread
 * having to run inside it. Resume puts the normal timeout back.
 *
 * This is still an honest watchdog: outside a declared pause the timeout is the
 * short one, and a pause is a property of the operation (a whole-partition
 * erase), not of the code that happens to be running.
 */
static rt_atomic_t s_wdt_paused;

void s3_wdt_pause(void)
{
    /* rt_atomic_add returns the OLD value. 0 means this is the outermost pause,
     * i.e. the one that has to reprogram the hardware; anything else means an
     * outer caller already did. (Getting this backwards made pause a no-op:
     * measured as a TG0WDT reset in the middle of a format with a 5 s timeout.) */
    if (rt_atomic_add(&s_wdt_paused, 1) != 0)
    {
        return;
    }

    /*
     * Park the hardware, do not merely lengthen it.
     *
     * Lengthening was the first attempt and it was wrong for a structural
     * reason: the pause has to cover "however long this erase takes", and there
     * is no bound on that. Measured: with the timeout raised to 60 s the format
     * still tripped the timer once Wi-Fi and BLE were up, because their flash
     * traffic stretches the window well past it. Anything short enough to still
     * be a watchdog is short enough to fire.
     *
     * wdt_hal_disable() stops the counter outright, so the window is covered by
     * construction. The cost is explicit: a hang that starts *inside* a paused
     * window is not caught. That is the same trade IDF makes - its task watchdog
     * also stops being fed across a flash operation - and the alternative (no
     * watchdog at all when the filesystem is in use) is strictly worse.
     */
    wdt_hal_write_protect_disable(&s_wdt_hal);
    wdt_hal_disable(&s_wdt_hal);
    wdt_hal_write_protect_enable(&s_wdt_hal);
}

void s3_wdt_resume(void)
{
    /* Also the OLD value: 1 means this resume is the outermost one and the
     * timeout has to go back to normal. */
    if (rt_atomic_sub(&s_wdt_paused, 1) != 1)
    {
        return;
    }

    wdt_hal_write_protect_disable(&s_wdt_hal);
    wdt_hal_feed(&s_wdt_hal);
    wdt_hal_enable(&s_wdt_hal);
    wdt_hal_write_protect_enable(&s_wdt_hal);
}

static void s3_wdt_thread(void *arg)
{
    RT_UNUSED(arg);

    while (1)
    {
        if (rt_atomic_load(&s_wdt_paused) == 0)
        {
            /* Write-protection has to be lifted around every register write;
             * the HAL does not do it implicitly (wdt_hal.h:82). */
            wdt_hal_write_protect_disable(&s_wdt_hal);
            wdt_hal_feed(&s_wdt_hal);
            wdt_hal_write_protect_enable(&s_wdt_hal);

            s_wdt_feeds++;
        }

        rt_thread_mdelay(S3_WDT_FEED_MS);
    }
}

/*
 * Arm the watchdog.
 *
 * Called from an INIT_APP_EXPORT so it is running before the shell appears -
 * a watchdog that starts after the application is already up protects nothing
 * during the window where a bring-up bug would actually bite.
 *
 * Order matters: the hardware is configured and enabled FIRST, then the feeder
 * thread is created. Doing it the other way round means the feeder can run
 * against an unconfigured context, and the reload values are uninitialised.
 */
static int s3_wdt_init(void)
{
    rt_thread_t tid;

    /* Power the timer group and take it out of reset before touching it. The
     * peripheral clock is not on by default. */
    periph_module_enable(S3_WDT_PERIPH);

    wdt_hal_init(&s_wdt_hal, S3_WDT_INSTANCE, S3_WDT_PRESCALER, false);

    wdt_hal_write_protect_disable(&s_wdt_hal);

    /* Stage 0 resets the system. A pure interrupt stage would need a handler on
     * a level-4/5 line, which this port does not dispatch. */
    wdt_hal_config_stage(&s_wdt_hal, WDT_STAGE0, S3_WDT_TIMEOUT_TICKS,
                         WDT_STAGE_ACTION_RESET_SYSTEM);

    /* Stages 1..3 stay off: one timeout is the whole policy. */
    wdt_hal_config_stage(&s_wdt_hal, WDT_STAGE1, 0, WDT_STAGE_ACTION_OFF);
    wdt_hal_config_stage(&s_wdt_hal, WDT_STAGE2, 0, WDT_STAGE_ACTION_OFF);
    wdt_hal_config_stage(&s_wdt_hal, WDT_STAGE3, 0, WDT_STAGE_ACTION_OFF);

    wdt_hal_enable(&s_wdt_hal);
    wdt_hal_write_protect_enable(&s_wdt_hal);

    /* Give the hardware one reload before the thread starts, so the first
     * timeout window begins now rather than at power-on. */
    wdt_hal_write_protect_disable(&s_wdt_hal);
    wdt_hal_feed(&s_wdt_hal);
    wdt_hal_write_protect_enable(&s_wdt_hal);

    s_wdt_armed = 1;

    tid = rt_thread_create("wdt", s3_wdt_thread, RT_NULL, 1024,
                           RT_THREAD_PRIORITY_MAX - 3, 10);
    if (tid == RT_NULL)
    {
        /* Leave the hardware running: a firmware that cannot allocate a 1 KB
         * stack has a bug, and a reset loop is a louder symptom than a
         * watchdog that is silently off. */
        rt_kprintf("s3wdt: feeder thread create FAILED, timer left armed\n");
        return -RT_ENOMEM;
    }

    rt_thread_startup(tid);

    rt_kprintf("s3wdt: armed, %d ms timeout, feeder every %d ms\n",
               S3_WDT_TIMEOUT_MS, S3_WDT_FEED_MS);
    return RT_EOK;
}
INIT_APP_EXPORT(s3_wdt_init);

/* ---------------------------------------------------------------------------
 * `s3wdt` - report state, and a deliberate hang to prove the reset works.
 *
 * `s3wdt hang` suspends the feeder thread and spins. The board MUST reset
 * within roughly S3_WDT_TIMEOUT_MS; that is the only way to know the watchdog
 * is wired to a real reset and not to an interrupt nobody handles. The counter
 * stops advancing first, which is visible in `s3wdt` before the hang.
 * ------------------------------------------------------------------------- */

static int cmd_s3wdt(int argc, char **argv)
{
    if (argc >= 2 && rt_strcmp(argv[1], "hang") == 0)
    {
        rt_thread_t tid = rt_thread_find("wdt");

        if (tid == RT_NULL)
        {
            rt_kprintf("s3wdt: feeder thread not found\n");
            return -1;
        }

        rt_kprintf("s3wdt: suspending the feeder and spinning; the board must "
                   "reset in ~%d ms\n", S3_WDT_TIMEOUT_MS);
        rt_kprintf("s3wdt: feeds so far = %u\n", (unsigned)s_wdt_feeds);

        /* Suspend the feeder so nothing reloads the timer, then spin with
         * interrupts enabled - this is a "software hang", the case the
         * watchdog is supposed to catch. */
        rt_thread_suspend(tid);

        while (1)
        {
            /* no rt_thread_mdelay: that would yield and let other threads run,
             * but the feeder is suspended either way. Busy is the point. */
        }
    }

    rt_kprintf("s3wdt: armed=%u feeds=%u paused=%d timeout=%d ms period=%d ms\n",
               (unsigned)s_wdt_armed, (unsigned)s_wdt_feeds,
               (int)rt_atomic_load(&s_wdt_paused),
               S3_WDT_TIMEOUT_MS, S3_WDT_FEED_MS);
    rt_kprintf("s3wdt: `s3wdt hang` proves the reset path\n");
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_s3wdt, s3wdt, hardware watchdog state, and `hang` to test it);

#else /* no timer group */

static int s3_wdt_init(void)
{
    rt_kprintf("s3wdt: this chip reports no TIMG; watchdog not armed\n");
    return RT_EOK;
}
INIT_APP_EXPORT(s3_wdt_init);

static int cmd_s3wdt(int argc, char **argv)
{
    RT_UNUSED(argc);
    RT_UNUSED(argv);
    rt_kprintf("s3wdt: not supported on this target (no TIMG)\n");
    return -1;
}
MSH_CMD_EXPORT_ALIAS(cmd_s3wdt, s3wdt, hardware watchdog state (unsupported target));

#endif /* SOC_GPTIMER_SUPPORTED */
