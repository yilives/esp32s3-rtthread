/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-11-04     RT-Thread    first version, ESP32-S3 SMP bring-up
 *
 * ESP32-S3 SMP: the inter-core interrupt and the second core's entry point.
 *
 * Everything in this file is ESP32-specific, which is why it is here and not in
 * libcpu/xtensa: PRID, the SYSTEM_CPU_INTR_FROM_CPU_n registers and the ROM
 * boot-address call are all Espressif. libcpu keeps the parts that any Xtensa
 * would need (spinlocks, per-core state, the entry/exit vectors).
 *
 * How CPU1 gets here
 * ------------------
 * The port does NOT start CPU1. ESP-IDF does, from
 * esp_system/port/cpu_start.c start_other_core(), which runs before the
 * RT-Thread scheduler exists:
 *
 *     esp_cpu_unstall(1)
 *     cpu_utility_ll_enable_clock_and_reset_app_cpu()
 *     ets_set_appcpu_boot_addr((uint32_t)call_start_cpu1)
 *
 * and then busy-waits for s_cpu_up[1]. call_start_cpu1() does the per-core
 * Xtensa setup the hardware does not do for a core that was just released -
 * init_pre_rtos_tls_area(1), esp_cpu_intr_set_ivt_addr(&_vector_table) - and
 * finally hands control to start_cpu_other_cores(), which under RT-Thread waits
 * for s_system_full_inited and calls rt_secondary_cpu_entry().
 *
 * That is the function this file provides. It is the counterpart of the
 * `start_kernel_early` path in libcpu/risc-v/common/startup_gcc.S, and it has
 * exactly the same job: bring this core's own state up, then enter the
 * scheduler.
 *
 * Why the second core needs its own copy of everything
 * ----------------------------------------------------
 * On Xtensa, INTENABLE and VECBASE are *special registers*, i.e. per core
 * (components/xtensa/include/xt_utils.h:112 "rsr INTENABLE",
 * esp_cpu_intr_set_ivt_addr "the current CPU's IVT"). The interrupt handler
 * table is per core too: xt_set_interrupt_handler() indexes it with
 * `n * portNUM_PROCESSORS + xPortGetCoreID()`. So an interrupt must be
 * *allocated on the core that will receive it* - esp_intr_alloc's own header
 * says "The interrupt will always be allocated on the core that runs this
 * function", and it hard-rejects a mismatch (intr_alloc.c:701). Hence
 * rt_xt_ipi_init() being called once per core, on that core.
 *
 * The IPI line
 * ------------
 * ESP32-S3 has four independent CPU-to-CPU lines. IDF takes line 0 for core 0
 * and line 1 for core 1 (crosscore_int.c:100-105), so this port takes lines 2
 * and 3 - the ones the SoC description already reserves for an IPC ISR:
 *
 *     ETS_FROM_CPU_INTR2_SOURCE = 81   "Used for IPC_ISR"
 *     ETS_FROM_CPU_INTR3_SOURCE = 82   "Used for IPC_ISR"
 *
 * The mapping is one line per core, not one line shared: a source routed
 * through the interrupt matrix reaches every core it is routed to, so a shared
 * line could not target a single core. Register n triggers source 79+n and is
 * written by the *sender*; the receiver clears its own register. That is the
 * same convention IDF uses, just on different lines.
 */

#include <rtthread.h>
#include <rthw.h>

#include "sdkconfig.h"

#ifdef RT_USING_SMP

/*
 * STATUS: builds, links, and CPU1 really does enter the kernel - the dual-core
 * boot shows the two cores' output interleaved character by character, which is
 * only possible if both are executing this file's rt_secondary_cpu_entry().
 *
 * It does not reach a shell, and the blocker is NOT in this file: with CPU1
 * parked before the scheduler the same hang happens, so the first context switch
 * out of rt_system_scheduler_start() on CPU0 is what stops. See
 * bsp/ESP/ESP32_S3/rtconfig.h for the full state before turning this on.
 */

#include "esp_attr.h"
#include "esp_intr_alloc.h"
#include "esp_rom_sys.h"
#include "soc/system_reg.h"
#include "soc/system_intr.h"
#include "soc/interrupts.h"

/* Defined in esp_system/crosscore_int.c: registers this core's inter-core
 * interrupt. Declared here rather than pulling in the private header. */
extern void esp_crosscore_int_init(void);

/* Provided by libcpu/xtensa (Xtensa HAL) and by this BSP's board.c. Neither is
 * declared in a header this file includes, and both are needed per core. */
extern void _xt_coproc_init(void);
extern void rt_hw_xtensa_fpu_init(void);
extern void vPortSetupTimer(void);

/*
 * The IPI line this port owns, one per core: FROM_CPU_0 for core 0, FROM_CPU_1
 * for core 1.
 *
 * NOT FROM_CPU_2/3. IDF's own crosscore code
 * (components/esp_system/crosscore_int.c) registers SYS_CPU_INTR_FROM_CPU_0_SOURCE
 * on core 0 and SYS_CPU_INTR_FROM_CPU_1_SOURCE on core 1, and
 * crosscore_int_ll_trigger_interrupt() writes FROM_CPU_0_REG to reach core 0 and
 * FROM_CPU_1_REG to reach core 1. The numbering is "register n delivers to core
 * n", so registers 2 and 3 target cores 2 and 3 - which this chip does not have.
 *
 * Measured with 2/3: the write DID latch (read back 1 from both cores, level
 * line held high) and INTENABLE had the line enabled, yet the handler never ran -
 * `IS ipi=0 ... en=12000007 ... init1=a5a5a5a5`. A level line that stays
 * asserted while the ISR count stays zero can only mean the source is not routed
 * to any core at all.
 */
#define XT_RT_IPI_SOURCE(cpu)   (SYS_CPU_INTR_FROM_CPU_0_SOURCE + (cpu))

/* One register per target core. */
#define XT_RT_IPI_REG(cpu)      ((cpu) == 0 ? SYSTEM_CPU_INTR_FROM_CPU_0_REG \
                                            : SYSTEM_CPU_INTR_FROM_CPU_1_REG)

/* The ROM entry that programs APPCPU_CTRL_REG_D. Declared here because the
 * public esp_rom header that carries it is per-chip and this file is already
 * ESP32-S3 specific. components/esp_rom/esp32s3/ld/esp32s3.rom.ld:51 pins it. */
extern void ets_set_appcpu_boot_addr(unsigned int addr);

/* Provided by ESP-IDF's components/esp_hw_support/cpu.c. */
extern void esp_cpu_unstall(int core_id);

static volatile rt_uint32_t s_ipi_count[RT_CPUS_NR];

/*
 * The IPI handler.
 *
 * IRAM_ATTR because the line can fire while the flash cache is disabled (CPU0
 * inside a spi_flash operation). The body is deliberately minimal, and it does
 * NOT call rt_schedule() itself: the entry/exit vector pair already funnels
 * every level-1 exit through _rtt_int_exit, which performs a pending switch.
 * Setting the flag through rt_schedule() from here is the documented way to
 * request one, and it is what portYIELD_FROM_ISR() resolves to on this port.
 */
static void IRAM_ATTR xt_rt_ipi_isr(void *arg)
{
    int cpu = (int)(rt_ubase_t)arg;

    /* Clear first: the line is level-triggered, so leaving it asserted would
     * re-enter this handler forever. */
    WRITE_PERI_REG(XT_RT_IPI_REG(cpu), 0);

    if (cpu >= 0 && cpu < RT_CPUS_NR)
    {
        s_ipi_count[cpu]++;
    }

    /* Ask for a reschedule on this core. Outside an interrupt this would switch
     * immediately; inside one it records the request and _rtt_int_exit acts on
     * it, which is exactly the behaviour an IPI wants. */
    rt_schedule();
}

/*
 * Route this core's IPI line and unmask it. Must run ON the core that will
 * receive it - see the file header.
 */
int rt_xt_ipi_init(void)
{
    int cpu = rt_hw_cpu_id();
    esp_err_t err;

    if (cpu < 0 || cpu >= RT_CPUS_NR)
    {
        return -1;
    }

    /* Flags: level 1, in IRAM.
     *
     * LEVEL1 and not higher: the RT-Thread port dispatches every handler from
     * the level-1 vector (see libcpu/xtensa/context_gcc.S), so a handler
     * installed on a level-2+ line would run in a context where _rtt_int_enter
     * is never called and no switch can be requested.
     *
     * IRAM: the line can be raised by the other core at any time, including
     * while this core has the cache off. */
    {
        intr_handle_t handle = RT_NULL;

        err = esp_intr_alloc(XT_RT_IPI_SOURCE(cpu),
                             ESP_INTR_FLAG_LEVEL1 | ESP_INTR_FLAG_IRAM,
                             xt_rt_ipi_isr, (void *)(rt_ubase_t)cpu, &handle);
        if (err != ESP_OK)
        {
            rt_kprintf("xt-ipi: core %d, esp_intr_alloc(0x%x) failed (%s)\n",
                       cpu, XT_RT_IPI_SOURCE(cpu), esp_err_to_name(err));
            return -1;
        }

        /*
         * Enable it explicitly, on top of the allocation.
         *
         * On CPU0 this runs from rt_hw_board_init(), which is earlier than
         * ESP-IDF's own interrupt plumbing gets a chance to unmask anything -
         * measured: allocation returned ESP_OK, yet that core's INTENABLE read
         * back as 0x00000002 (systimer only), so the line was registered but
         * never armed and every IPI sent to it was silently dropped. CPU1 does
         * the same call later in its secondary entry and read back
         * 0x12000007, which is why only CPU1 ever received interrupts.
         */
        if (handle != RT_NULL)
        {
            esp_intr_enable(handle);
        }
    }

    s_ipi_count[cpu] = 0;

    return 0;
}

/* Diagnostic: how many IPIs this core has taken. */
rt_uint32_t rt_xt_ipi_count(int cpu)
{
    if (cpu < 0 || cpu >= RT_CPUS_NR)
    {
        return 0;
    }

    return s_ipi_count[cpu];
}

/*
 * Send an IPI to the cores named by cpu_mask.
 *
 * The only vector RT-Thread uses is RT_SCHEDULE_IPI (src/scheduler_mp.c:329,
 * 356, 1552, 1565), whose meaning is "reschedule yourself". There is nothing to
 * encode, so the vector argument is checked and otherwise unused.
 *
 * A write of bit 0 to the target's register latches the line; there is no
 * read-modify-write here and none is needed, because each core owns exactly one
 * register. IRAM_ATTR matches the callers, which include
 * rt_hw_context_switch_interrupt paths.
 */
void IRAM_ATTR rt_hw_ipi_send(int ipi_vector, unsigned int cpu_mask)
{
    RT_UNUSED(ipi_vector);

    /* One register per target core, named after the core it targets. */
    if (cpu_mask & (1u << 0))
    {
        WRITE_PERI_REG(XT_RT_IPI_REG(0), 1);
    }

    if (RT_CPUS_NR > 1 && (cpu_mask & (1u << 1)))
    {
        WRITE_PERI_REG(XT_RT_IPI_REG(1), 1);
    }
}

/*
 * CPU1's entry point, called by ESP-IDF.
 *
 * components/esp_system/startup.c:185 calls this from
 * start_cpu_other_cores_default(), after CPU0 has published
 * s_system_full_inited and *before* CPU0 has entered its scheduler.
 * startup.c:187 aborts if it ever returns, so this is noreturn.
 *
 * The four steps, and why they are in this order:
 *
 *   1. rt_xt_cpu_init()      resolve this core's &irq_nest. Must come before
 *                            any interrupt can be delivered, because the vector
 *                            writes through that pointer.
 *   2. rt_xt_ipi_init()      allocate the IPI on THIS core. Doing it from CPU0
 *                            would return ESP_ERR_INVALID_STATE
 *                            (intr_alloc.c:701).
 *   3. rt_hw_spin_lock(&_cpus_lock)
 *                            CPU0 takes this lock in startup.c:294 and
 *                            rt_system_scheduler_start() releases it
 *                            (scheduler_mp.c:504) as its first statement. Taking
 *                            it here therefore blocks until CPU0 has reached
 *                            that point, and keeps the two cores out of the
 *                            picker at the same time. It also balances the
 *                            unconditional unlock inside
 *                            rt_system_scheduler_start().
 *   4. rt_system_scheduler_start()
 *                            picks this core's idle thread and switches to it.
 *                            It does not return.
 *
 * IRAM_ATTR: entered from an IRAM function while CPU0 may still be finishing
 * its own init.
 */


void IRAM_ATTR rt_secondary_cpu_entry(void)
{
    rt_xt_cpu_init();

    /*
     * IDF's inter-core interrupt is deliberately NOT initialised here.
     *
     * esp_crosscore_int_init() allocates SYS_CPU_INTR_FROM_CPU_1_SOURCE on core 1
     * - the same line this port now uses for its own scheduling IPI. Two
     * handlers, one line: whichever registers second gets ESP_ERR_INVALID_STATE,
     * or the two share and neither works.
     *
     * Nothing else on this image needs crosscore: esp_ipc delivers through a
     * task notification (esp_ipc.c:193), not through this interrupt. Round 58
     * added the call on the theory that NVS needed it; the measurements in
     * rounds 62-70 showed the actual dependency was the notification, and this
     * line was only ever competing for the source.
     */

    /*
     * Per-core coprocessor state, before any interrupt can arrive.
     *
     * rt_hw_board_init() runs _xt_coproc_init() and the FPU enable, but it runs
     * on CPU0 only. _xt_coproc_init() fills _xt_coproc_owner_sa[cpu] for the
     * *calling* core (cpuport.c:362-389), so CPU1's owner table and CPENABLE
     * were left at whatever the boot ROM had - and _xt_context_save/
     * _xt_context_restore both touch the FP block. That is the most likely
     * reason CPU1's very first tick died inside _xt_context_save with a double
     * exception while CPU0's identical code path survives.
     *
     * Same order as rt_hw_board_init() uses.
     */
    _xt_coproc_init();
    rt_hw_xtensa_fpu_init();

    /*
     * This core needs its own tick.
     *
     * vSystimerSetup() (port_systick.c:69-86) allocates the SYSTIMER interrupt
     * on the core that RUNS it, and vPortSetupTimer() is called once, from
     * rt_hw_board_init() on CPU0. Without this, CPU1 programs nothing, never
     * takes a tick, and therefore never calls rt_schedule() from an interrupt -
     * so _rtt_resume never runs here. Measured with the s3smp counters:
     *
     *   [SM] cpu0 lock=272 unlock=248 post=1 sw=1
     *   [SM] cpu1 lock=5   unlock=4   post=0 sw=0
     *
     * sw=0 is the whole story: this core has never performed a context switch,
     * so nothing ever calls rt_sched_post_ctx_switch() here, so the outgoing
     * thread's critical_lock_nest is never cleared, so every blocking call from
     * this core's threads is refused for the rest of time.
     */
    vPortSetupTimer();

    rt_xt_ipi_init();



    /* This core is about to be scheduled for the first time. */
    rt_hw_local_irq_disable();

    /*
     * Do NOT take the legacy _cpus_lock here.
     *
     * rt_system_scheduler_start() begins with an UNCONDITIONAL
     * rt_hw_spin_unlock(&_cpus_lock) (scheduler_mp.c:504) - it assumes the BSP
     * took the lock once during startup and hands it back on the way into the
     * first thread. CPU0 does take it (esp_system/startup.c:294-295), so on
     * CPU0 the take and the release balance.
     *
     * Taking it again on CPU1 has no matching release: CPU1's
     * rt_system_scheduler_start() still performs that unlock, but it now
     * cancels *this* take instead of the one CPU0 made, leaving the lock held
     * with no owner. Measured: `_cpus_lock == 2` (this port stores core+1 in
     * the owner word) while both cores sat in the scheduler, and the board
     * printed the banner and then stopped with no msh prompt. Removing the take
     * is what finally produced `msh >`.
     *
     * The ordering it was there for - "CPU1 must not enter the ready-queue
     * picker while CPU0 is in it" - is already provided by the take inside
     * rt_system_scheduler_start() itself (_fast_spin_lock(&_mp_scheduler_lock)),
     * which both cores go through. */

    rt_system_scheduler_start();

    /* Not reached: rt_system_scheduler_start() switches away. If it ever does
     * return, IDF's startup.c logs "RT-Thread secondary CPU entry returned" and
     * aborts, which is the right outcome - quietly continuing on a core with no
     * thread would be worse. */
    for (;;)
    {
        ;
    }
}

/*
 * Start CPU1, the RT-Thread way.
 *
 * NOT used by the ESP-IDF build: startup.c:287 says so explicitly - "IDF has
 * already brought up the other cores, so do not invoke rt_hw_secondary_cpu_up()
 * again". It is provided because rthw.h declares it under RT_USING_SMP and
 * because a non-IDF build of this port (the scons route) has no IDF startup to
 * do it.
 *
 * The sequence is IDF's start_other_core() with the boot address written
 * BEFORE the core is unstalled. IDF does it the other way round and explains
 * why (OpenOCD may already have unstalled the core); here that ordering would
 * let a half-released core fetch whatever the register still held, so the
 * address is armed first.
 */
void rt_hw_secondary_cpu_up(void)
{
    if (rt_hw_cpu_id() != 0)
    {
        return;
    }

    ets_set_appcpu_boot_addr((unsigned int)(rt_ubase_t)rt_secondary_cpu_entry);
    esp_cpu_unstall(1);
}

/*
 * What CPU1 runs when it has nothing to do.
 *
 * src/idle.c:121 calls this in a loop on every core except CPU0. `waiti` is the
 * Xtensa "wait for interrupt, low power" instruction: it stops the core until
 * any enabled interrupt arrives, which is what keeps an idle second core from
 * burning power and bus bandwidth. Level 0 means "wake on any interrupt at or
 * above level 0", i.e. all of them.
 */
void rt_hw_secondary_cpu_idle_exec(void)
{
    __asm__ volatile ("waiti 0" ::: "memory");
}

#endif /* RT_USING_SMP */
