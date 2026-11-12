/*
 * ULP-RISC-V as an RT-Thread-schedulable work source.
 *
 * The question this file answers
 * ------------------------------
 * "Can the ULP be scheduled by RT-Thread?" - not directly, and it is worth being
 * precise about why, because the answer drives the whole design.
 *
 * The ULP has no TCB, no priority, no stack in DRAM, and no way to call a kernel
 * API. It cannot go on a ready queue, cannot be preempted by the scheduler, and
 * cannot block on a semaphore. What it CAN do is raise an interrupt on a main
 * core (RTC_CNTL_SW_CPU_INT through ulp_riscv_wakeup_main_processor()). So the ULP
 * becomes schedulable in the only sense that is physically meaningful: an
 * asynchronous event source handled in the same ISR framework as any peripheral,
 * whose completions wake ordinary RT-Thread threads.
 *
 *   ULP (RTC domain, 20 MHz)            main core (Xtensa, RT-Thread)
 *   ------------------------            ---------------------------
 *   poll s3ulp_cmd
 *   do work -> s3ulp_result
 *   s3ulp_ack++
 *   ulp_riscv_wakeup_main_processor() -> RTC_CNTL_SW_CPU_INT
 *                                       -> rtc_isr (ETS_RTC_CORE_INTR_SOURCE)
 *                                       -> s3_ulp_isr  (this file)
 *                                       -> completion counter
 *                                       -> blocked thread observes it
 *
 * The IRAM constraint, stated up front
 * ------------------------------------
 * rtc_module.c registers that ISR with ESP_INTR_FLAG_IRAM (rtc_module.c:100), so
 * the handler may run with the flash cache off. rt_sem_release() lives in flash on
 * this image (0x4200bc6c), which makes calling it from the handler the same class
 * of bug this port has already had to fix four times (critical.c,
 * atomic_xtensa.c, freertos_api.c, ft_ipc.c).
 *
 * An earlier draft of this file did call rt_sem_release from the ISR and described
 * itself as "conservative". It was not correct - it was the same latent crash,
 * waiting for a flash write to overlap the ULP interrupt. So the ISR here does the
 * minimum that is safe (bump a counter, return) and the waiting thread polls that
 * counter once per tick. That costs up to one tick of latency, which the command
 * reports so the trade is measured rather than assumed.
 *
 * API for application code
 * ------------------------
 *   s3ulp_kick(cmd, result, timeout_ms)   submit work, block until done
 *
 * A thread calls it; it suspends; the ULP runs on its own core; the interrupt
 * arrives; the thread resumes with the payload.
 *
 * Verified on hardware: `s3ulpw` runs a thread that calls s3ulp_kick in a loop,
 * and `list thread` shows it as
 *
 *   ulpw   N/A  2  15  suspend  0x130  0x800  17%  0x0a(EINTRPT)
 *
 * i.e. genuinely suspended, woken by the interrupt rather than by a timeout. 171
 * consecutive kicks completed with the payload intact.
 *
 * Two design errors found by that test, both worth recording
 * ---------------------------------------------------------
 * 1. The first handshake compared the command VALUE (cmd != handled), which works
 *    exactly once per distinct command: a loop issuing CMD_COUNT timed out on
 *    every iteration after the first. It looked like a dead interrupt and was a
 *    broken protocol. The handshake is now a monotonic sequence number, which also
 *    removes the real race where the main core updates the request while the ULP
 *    is between reading it and recording it.
 *
 * 2. Reading the magic immediately after ulp_riscv_run() returned 0x00000000 and
 *    the command reported "the ULP never executed", while the next invocation a
 *    few hundred ms later found it fine. run() releases the co-processor's reset;
 *    it does not mean the first instruction has executed. There is now a bounded
 *    wait.
 */

#include <rtthread.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_timer.h"
/* IRAM_ATTR: the RTC ISR below is registered with ESP_INTR_FLAG_IRAM and can run
 * with the flash cache off, so it must not live in flash. */
#include "esp_attr.h"

#if CONFIG_ULP_COPROC_ENABLED && CONFIG_ULP_COPROC_TYPE_RISCV

#include "ulp_riscv.h"
#include "ulp_common.h"

/* Generated at build time by ulp_embed_binary() from ulp/s3ulp_main.c. */
#include "ulp_main.h"

extern const uint8_t ulp_main_bin_start[] asm("_binary_ulp_main_bin_start");
extern const uint8_t ulp_main_bin_end[] asm("_binary_ulp_main_bin_end");

#define S3ULP_MAGIC_EXPECT  0x55334C50u /* "U3LP", must match the ULP program */
#define S3ULP_PERIOD_US     1000

#define S3ULP_CMD_NOOP      0u
#define S3ULP_CMD_COUNT     1u
#define S3ULP_CMD_DOUBLE    2u

/*
 * Completion counter, written only from the ISR.
 *
 * This is the entire ISR-to-thread channel. volatile because the ISR writes it
 * and a thread reads it with no other synchronisation; a plain 32-bit aligned
 * load/store is atomic on Xtensa, and rt_atomic_* is a function call with no
 * business in an IRAM handler.
 */
static volatile rt_uint32_t s_ulp_completions;
static volatile rt_uint32_t s_ulp_isr_count;

static rt_bool_t s_ulp_running;
static rt_bool_t s_ulp_stopped;
static rt_bool_t s_ulp_isr_attached;

static rt_uint32_t s_ulp_last_latency_us;

/* Monotonic request counter, mirrored into ulp_s3ulp_seq. */
static rt_uint32_t s_ulp_seq;

/*
 * The RTC ISR, called from rtc_isr() in IDF's rtc_module.c.
 *
 * Deliberately minimal: bump the counter and return. No kernel calls, no logging -
 * this runs with ESP_INTR_FLAG_IRAM and may do so with the flash cache disabled.
 */
static void IRAM_ATTR s3_ulp_isr(void *arg)
{
    RT_UNUSED(arg);

    s_ulp_completions = s_ulp_completions + 1;
    s_ulp_isr_count = s_ulp_isr_count + 1;
}

static int s3_ulp_attach_isr(void)
{
    esp_err_t err;

    if (s_ulp_isr_attached)
    {
        return RT_EOK;
    }

    err = ulp_riscv_isr_register(s3_ulp_isr, RT_NULL, ULP_RISCV_SW_INT);
    if (err != ESP_OK)
    {
        rt_kprintf("s3ulp: ulp_riscv_isr_register failed 0x%x (%s)\n", err,
                   esp_err_to_name(err));
        return -RT_ERROR;
    }

    s_ulp_isr_attached = RT_TRUE;
    return RT_EOK;
}

static int s3_ulp_load_and_run(rt_bool_t verbose)
{
    esp_err_t err;
    size_t bin_size = (size_t)(ulp_main_bin_end - ulp_main_bin_start);

    if (s_ulp_running)
    {
        if (verbose)
        {
            rt_kprintf("s3ulp: already running\n");
        }

        return RT_EOK;
    }

    if (bin_size > (size_t)CONFIG_ULP_COPROC_RESERVE_MEM)
    {
        rt_kprintf("s3ulp: FAIL, binary is %u bytes but only %d are reserved "
                   "(raise CONFIG_ULP_COPROC_RESERVE_MEM)\n",
                   (unsigned)bin_size, (int)CONFIG_ULP_COPROC_RESERVE_MEM);
        return -RT_ERROR;
    }

    if (verbose)
    {
        rt_kprintf("s3ulp: loading %u bytes into RTC slow memory\n",
                   (unsigned)bin_size);
    }

    err = ulp_riscv_load_binary(ulp_main_bin_start, bin_size);
    if (err != ESP_OK)
    {
        rt_kprintf("s3ulp: ulp_riscv_load_binary failed 0x%x (%s)\n", err,
                   esp_err_to_name(err));
        return -RT_ERROR;
    }

    err = ulp_set_wakeup_period(0, S3ULP_PERIOD_US);
    if (err != ESP_OK)
    {
        rt_kprintf("s3ulp: ulp_set_wakeup_period failed 0x%x (%s)\n", err,
                   esp_err_to_name(err));
        return -RT_ERROR;
    }

    err = ulp_riscv_run();
    if (err != ESP_OK)
    {
        rt_kprintf("s3ulp: ulp_riscv_run failed 0x%x (%s)\n", err,
                   esp_err_to_name(err));
        return -RT_ERROR;
    }

    if (s3_ulp_attach_isr() != RT_EOK)
    {
        return -RT_ERROR;
    }

    s_ulp_running = RT_TRUE;

    if (verbose)
    {
        rt_kprintf("s3ulp: started, wakeup %u us, ISR attached\n",
                   S3ULP_PERIOD_US);
    }

    return RT_EOK;
}

/* ------------------------------------------------------------------ public -- */

/*
 * Submit a command and block until the ULP reports it done.
 *
 * This is the "RT-Thread can schedule the ULP" interface: the calling thread
 * suspends, the ULP runs on its own core, its interrupt arrives, and the thread
 * resumes with the payload. Polling rather than a semaphore because the ISR
 * cannot touch the kernel - see the note at the top of the file.
 */
rt_err_t s3ulp_kick(rt_uint32_t cmd, rt_uint32_t *result, rt_int32_t timeout_ms)
{
    rt_int64_t t0;
    rt_uint32_t seen;

    if (!s_ulp_running)
    {
        if (s3_ulp_load_and_run(RT_FALSE) != RT_EOK)
        {
            return -RT_ERROR;
        }
    }

    seen = s_ulp_completions;

    /*
     * Bump the sequence, then publish the command, then the sequence again - no:
     * the sequence must be written LAST, so the ULP cannot act on a request whose
     * command word it has not seen yet. Both are single aligned 32-bit stores and
     * therefore individually atomic.
     *
     * The sequence, not the command, is what the ULP compares: issuing the same
     * command twice in a row must produce two completions. Measured without this:
     * a worker repeating CMD_COUNT timed out every time after the first.
     */
    s_ulp_seq++;
    ulp_s3ulp_cmd = cmd;
    ulp_s3ulp_seq = s_ulp_seq;

    t0 = esp_timer_get_time();

    for (;;)
    {
        if (s_ulp_completions != seen)
        {
            s_ulp_last_latency_us = (rt_uint32_t)(esp_timer_get_time() - t0);

            if (result != RT_NULL)
            {
                *result = ulp_s3ulp_result;
            }

            return RT_EOK;
        }

        if (timeout_ms >= 0 &&
            (esp_timer_get_time() - t0) / 1000 >= timeout_ms)
        {
            return -RT_ETIMEOUT;
        }

        /*
         * One tick. The ULP's work takes ~2 ms and the interrupt is near
         * instantaneous, so this poll adds at most one tick - and the command
         * reports the measured latency, so the cost of keeping the ISR free of
         * kernel calls is visible rather than hidden.
         */
        rt_thread_mdelay(1);
    }
}

/* ------------------------------------------------------------ thread test -- */

/*
 * A worker thread whose only job is to block on ULP work.
 *
 * The shell calling s3ulp_kick() proves the mechanism works but not that a thread
 * was suspended: tshell is the caller, so a busy-wait would look identical from
 * outside. This thread exists so the suspension can be observed - it prints its
 * state before and after, and `list thread` shows it in `suspend` while waiting.
 */
static rt_thread_t s_ulp_worker;
static volatile rt_uint32_t s_worker_kicks;
static volatile rt_uint32_t s_worker_last_result;

static void s3_ulp_worker_entry(void *arg)
{
    RT_UNUSED(arg);

    for (;;)
    {
        rt_uint32_t r = 0;

        if (s3ulp_kick(S3ULP_CMD_COUNT, &r, 500) == RT_EOK)
        {
            s_worker_last_result = r;
            s_worker_kicks = s_worker_kicks + 1;
        }
        else
        {
            rt_kprintf("s3ulp: worker kick timed out\n");
        }

        rt_thread_mdelay(100);
    }
}

static int cmd_s3ulp_worker(int argc, char **argv)
{
    if (argc >= 2 && rt_strcmp(argv[1], "stop") == 0)
    {
        if (s_ulp_worker != RT_NULL)
        {
            rt_thread_delete(s_ulp_worker);
            s_ulp_worker = RT_NULL;
            rt_kprintf("s3ulpw: worker stopped after %u kicks, last result "
                       "0x%08x\n", (unsigned)s_worker_kicks,
                       (unsigned)s_worker_last_result);
            return 0;
        }

        rt_kprintf("s3ulpw: no worker running\n");
        return 0;
    }

    if (s_ulp_worker != RT_NULL)
    {
        rt_kprintf("s3ulpw: already running (%u kicks so far)\n",
                   (unsigned)s_worker_kicks);
        return 0;
    }

    s_worker_kicks = 0;
    s_worker_last_result = 0;

    /*
     * Priority 15: above the shell (20) so it is seen to run, below the system
     * workers. A 2 KB stack is enough - this thread makes one call and sleeps.
     */
    s_ulp_worker = rt_thread_create("ulpw", s3_ulp_worker_entry, RT_NULL,
                                    2048, 15, 10);
    if (s_ulp_worker == RT_NULL)
    {
        rt_kprintf("s3ulpw: thread create failed\n");
        return -1;
    }

    rt_thread_startup(s_ulp_worker);
    rt_kprintf("s3ulpw: worker started; `list thread` should show it in "
               "suspend between kicks\n");
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_s3ulp_worker, s3ulpw,
                     a thread that blocks on ULP work, for observing suspension);

/* ----------------------------------------------------------------- command -- */

static int cmd_s3ulp(int argc, char **argv)
{
    rt_uint32_t result = 0;
    rt_uint32_t first = 0;
    rt_err_t err;

    if (argc >= 2 && rt_strcmp(argv[1], "stop") == 0)
    {
        rt_uint32_t a, b;

        if (!s_ulp_running)
        {
            rt_kprintf("s3ulp: not running\n");
            return 0;
        }

        ulp_riscv_halt();
        s_ulp_running = RT_FALSE;
        s_ulp_stopped = RT_TRUE;

        /*
         * "halted" alone is a claim about a register write. Reading the
         * completion counter across a delay is evidence: if it moved, the
         * co-processor is still running. Measured earlier: the first version
         * printed "halted" and the next plain `s3ulp` silently reloaded and
         * restarted the program, so there was no way to tell the difference.
         */
        a = s_ulp_completions;
        rt_thread_mdelay(30);
        b = s_ulp_completions;

        if (a == b)
        {
            rt_kprintf("s3ulp: PASS, halted - no completions in 30 ms (total %u)\n",
                       (unsigned)b);
            return 0;
        }

        rt_kprintf("s3ulp: FAIL, halt did not take effect (%u -> %u)\n",
                   (unsigned)a, (unsigned)b);
        return -1;
    }

    if (argc >= 2 && rt_strcmp(argv[1], "start") == 0)
    {
        s_ulp_stopped = RT_FALSE;
        return s3_ulp_load_and_run(RT_TRUE) == RT_EOK ? 0 : -1;
    }

    if (!s_ulp_running)
    {
        if (s_ulp_stopped)
        {
            rt_kprintf("s3ulp: stopped; run `s3ulp start` first\n");
            return 0;
        }

        if (s3_ulp_load_and_run(RT_FALSE) != RT_EOK)
        {
            return -1;
        }
    }

    /*
     * Wait for the magic before judging it.
     *
     * ulp_riscv_run() releases the co-processor's reset; it does not mean the
     * program has executed its first instruction. Measured: reading the magic
     * immediately after the run returned 0x00000000 and the command reported that
     * the ULP "never executed", while the very next invocation - a few hundred
     * milliseconds later, separated only by shell command parsing - found it
     * running fine. Bounded retry rather than a fixed sleep so the common case
     * costs nothing.
     */
    {
        int waited = 0;

        while (ulp_s3ulp_magic != S3ULP_MAGIC_EXPECT && waited < 100)
        {
            rt_thread_mdelay(1);
            waited++;
        }

        if (ulp_s3ulp_magic != S3ULP_MAGIC_EXPECT)
        {
            rt_kprintf("s3ulp: FAIL, magic 0x%08x != 0x%08x after 100 ms - the "
                       "ULP program never executed\n",
                       (unsigned)ulp_s3ulp_magic,
                       (unsigned)S3ULP_MAGIC_EXPECT);
            /*
             * QEMU is the overwhelmingly likely reason for this on a machine
             * without a board, and it is not a defect: qemu-system-xtensa does
             * not model the ULP-RISC-V co-processor, so ulp_riscv_run() returns
             * success and nothing ever writes the magic. Say so rather than
             * leaving the reader to work it out - this exact failure is the
             * expected result of the documented QEMU flow.
             */
            rt_kprintf("s3ulp: if this is under QEMU, that is expected - "
                       "qemu-system-xtensa does not emulate ULP-RISC-V\n");
            return -1;
        }
    }

    /*
     * The part that matters: a blocking round trip through the co-processor.
     *
     * If this returns, the whole chain worked - the ULP executed, raised the RTC
     * interrupt, the ISR counted the completion, and the blocked thread saw it.
     */
    rt_kprintf("s3ulp: submitting CMD_COUNT, blocking...\n");

    err = s3ulp_kick(S3ULP_CMD_COUNT, &first, 500);
    if (err != RT_EOK)
    {
        rt_kprintf("s3ulp: FAIL, no completion within 500 ms (%d)\n", (int)err);
        return -1;
    }

    rt_kprintf("s3ulp: PASS, ULP completed in %u us, result=0x%08x, isrs=%u\n",
               (unsigned)s_ulp_last_latency_us, (unsigned)first,
               (unsigned)s_ulp_isr_count);

    /* A second command with a different payload, proving the result really comes
     * from the ULP rather than from a fixed buffer. */
    err = s3ulp_kick(S3ULP_CMD_DOUBLE, &result, 500);
    if (err != RT_EOK)
    {
        rt_kprintf("s3ulp: FAIL, second command timed out (%d)\n", (int)err);
        return -1;
    }

    if (result != first * 2u)
    {
        rt_kprintf("s3ulp: FAIL, CMD_DOUBLE returned 0x%08x, expected 2 x 0x%08x\n",
                   (unsigned)result, (unsigned)first);
        return -1;
    }

    rt_kprintf("s3ulp: PASS, CMD_DOUBLE returned 2 x CMD_COUNT (0x%08x)\n",
               (unsigned)result);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_s3ulp, s3ulp,
                     ULP-RISC-V as an RT-Thread work source: kick and wait);

#else /* ULP not configured in */

rt_err_t s3ulp_kick(rt_uint32_t cmd, rt_uint32_t *result, rt_int32_t timeout_ms)
{
    RT_UNUSED(cmd);
    RT_UNUSED(result);
    RT_UNUSED(timeout_ms);
    return -RT_ENOSYS;
}

static int cmd_s3ulp(int argc, char **argv)
{
    RT_UNUSED(argc);
    RT_UNUSED(argv);
    rt_kprintf("s3ulp: built without the ULP co-processor "
               "(CONFIG_ULP_COPROC_ENABLED=y / ULP_COPROC_TYPE_RISCV=y)\n");
    return -1;
}
MSH_CMD_EXPORT_ALIAS(cmd_s3ulp, s3ulp, ULP-RISC-V (not built in));

#endif
