/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-11-03     RT-Thread    first version, ESP32-S3 (Xtensa LX7) port
 *
 * ESP32-S3 board and bring-up glue for the RT-Thread kernel.
 *
 * This file replaces port/common/board.c for the Xtensa target. The shared
 * board.c is RISC-V specific throughout (riscv/interrupt.h,
 * esprv_int_set_threshold(), the CLIC sweep in rt_hw_intr_mask_unhandled(),
 * the device-model drivers), so it cannot be shared with an Xtensa port; the
 * two are selected per architecture in CMakeLists.txt instead of being merged
 * behind #ifdefs.
 *
 * What CONFIG_IDF_RTOS_RTTHREAD removes from IDF's boot path, and what this
 * file therefore has to do (startup.c calls entry() instead of
 * esp_startup_start_app()):
 *
 *   vPortSetupTimer()        the SYSTIMER alarm that becomes rt_tick_increase()
 *                            - IDF sets it up from app_startup.c
 *   _xt_coproc_init()        the coprocessor ownership table
 *                            - IDF does it in xPortStartScheduler()
 *   CPENABLE = CP0           the FPU enable
 *                            - IDF does it lazily, on the first FP instruction
 *   rt_components_board_init()  the INIT_BOARD_EXPORT() band
 *
 * Not done here, on purpose:
 *
 *   esp_int_wdt_init()       the interrupt watchdog. It is a genuine loss (the
 *                            S31 port starts it) but it needs the level-4/5
 *                            timer path wired up first; it goes in after the
 *                            kernel boots, not before.
 *   esp_crosscore_int_init() single core, and RT-Thread does not use IDF's
 *                            inter-core interrupt.
 *
 * esp_timer_init() is different: it is required the moment Wi-Fi or Bluetooth
 * is linked in, because esp_coex calls esp_timer_create() from a SECONDARY init
 * function that runs before rt_hw_board_init(). It is therefore started from an
 * ESP_SYSTEM_INIT_FN at priority 99, near the bottom of this file.
 */

#include <rtthread.h>

#include "sdkconfig.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_rom_sys.h"
#include "soc/soc.h"
#include "soc/interrupts.h"
#include "port_systick.h"
/* esp_timer_init() is started from an ESP_SYSTEM_INIT_FN, which lives here. */
#include "esp_private/startup_internal.h"

#include <xtensa/config/core.h>
#include <xtensa/corebits.h>
#include <xtensa_context.h>

#include "cpuport.h"

/* Coprocessor/exception support from ESP-IDF's components/xtensa. */
extern void _xt_coproc_init(void);

/* ---- FPU enable ---------------------------------------------------------- */

/*
 * Turn on CP0 (the single-precision FPU) and leave CP3 (AI/SIMD) off.
 *
 * Doing this once, here, is what lets the port skip IDF's lazy coprocessor
 * ownership machinery: the FPU is always "in use" from the exception vector's
 * point of view, and because context_gcc.S saves FCR/FSR/f0-f15 eagerly on
 * every outermost entry, correctness does not depend on knowing which thread
 * touched it last.
 *
 * This has to run before the first FP instruction of the image. The ROM's FP
 * usage is its own business, but anything GCC compiles from C may use `wfr`,
 * `madd.s` or an FP compare at any point, and those trap with
 * CoprocessorDisabled if CPENABLE says CP0 is off.
 */
void rt_hw_xtensa_fpu_init(void)
{
    __asm__ volatile ("movi a2, 1\n"
                      "wsr  a2, cpenable\n"
                      "rsync\n" ::: "a2", "memory");
}

/* ---- board init ---------------------------------------------------------- */

#ifdef RT_USING_SMP
extern int rt_xt_ipi_init(void);
#endif

void rt_hw_board_init(void)
{
    /*
     * Reuse IDF's OS tick alarm. vPortSetupTimer() routes
     * ETS_SYSTIMER_TARGET0_INTR_SOURCE through esp_intr_alloc() to
     * SysTickIsrHandler(), whose CONFIG_IDF_RTOS_RTTHREAD branch calls
     * rt_tick_increase(). RT_TICK_PER_SECOND must match CONFIG_FREERTOS_HZ;
     * port_systick.c has a _Static_assert on that.
     *
     * Calling it here means the alarm starts ticking before the scheduler
     * exists. That is safe and intentional: PS.INTLEVEL is raised
     * (rtthread_startup() disables interrupts first) so the interrupt cannot be
     * taken, and the first thread's rfe lowers the level and lets it fire.
     */
    vPortSetupTimer();

    /* The thread that is about to run is the one that owns the FPU. */
    _xt_coproc_init();
    rt_hw_xtensa_fpu_init();

#ifdef RT_USING_SMP
    /*
     * Resolve THIS core's per-core pointers, before the tick can fire.
     *
     * rt_xt_cpu_init() caches two addresses into rt_xt_core[cpu]: where the
     * kernel keeps its interrupt nesting counter, and where it keeps
     * pcpu->current_thread. The vector entry writes the first on every
     * interrupt and the switch path writes the second on every switch, both
     * without a function call - so both must be resolved before either can
     * happen.
     *
     * It was only being called from CPU1's entry, and the omission on CPU0 was
     * not visible until the dual-core build:
     *
     *   - the nesting counter was never incremented, so
     *     rt_tick_increase()'s RT_ASSERT(rt_interrupt_get_nest() > 0) fired on
     *     the first tick, mid-banner, and the boot died there;
     *   - current_thread was never published, so rt_thread_self() and
     *     rt_critical_level() reported "no thread" for a thread that was
     *     running.
     *
     * CPU0 is where the scheduler starts, so this has to happen here rather
     * than being left to the secondary entry.
     */
    rt_xt_cpu_init();


#endif

    /*
     * Console RX interrupt. Before the BOARD stage, so any board-stage
     * initialiser that prints is already talking to a live ring, and before the
     * scheduler, which is fine - the handler only fills a ring.
     *
     * Both failure paths below are reported and survived: the getchar poll in
     * console.c still delivers input when the line is unavailable, so a broken
     * interrupt path costs interactivity under load, not the console. What it
     * must not do is fail silently, which is why the return value is checked
     * and the mask state is printed - a line that is routed but left masked
     * never raises anything, and the only symptom would be `s3rt irq` showing
     * zero.
     */
    {
        extern rt_err_t rt_hw_console_irq_init(void);
        extern rt_bool_t rt_hw_console_irq_ok(void);

        if (rt_hw_console_irq_init() != RT_EOK)
        {
            rt_kprintf("board: console RX interrupt not installed, input is polled\n");
        }
        else if (!rt_hw_console_irq_ok())
        {
            rt_kprintf("board: console line %d still masked, input is polled\n",
                       ETS_UART0_INUM);
        }
    }

    /* Heap: already initialised by esp_system/startup.c do_core_init(), which
     * calls rt_system_heap_init(&__heap_start__, &__heap_end__) before any
     * CORE-stage init function runs. Re-initialising it here would wipe the
     * objects those functions already allocated, so this is deliberately not
     * repeated - the same reason port/common/board.c does not do it either. */

    /*
     * BOARD stage of the auto-init table. Must be the last thing in this
     * function: everything above is what a board-stage initialiser may rely
     * on (tick configured, FPU on, heap present).
     */
#ifdef RT_USING_COMPONENTS_INIT
    rt_components_board_init();
#endif
}

/* ---- time ---------------------------------------------------------------- */

/*
 * Strong definition of the weak stub in src/kservice.c. Without it every
 * rt_hw_us_delay() call becomes a one-shot warning and no delay at all, which
 * matters as soon as a driver does a reset pulse.
 */
void rt_hw_us_delay(rt_uint32_t us)
{
    esp_rom_delay_us(us);
}

/*
 * Reset hook for the architecture layer.
 *
 * libcpu/xtensa/cpuport.c implements rt_hw_cpu_reset() as "call this, then
 * park", because how a chip is restarted is board policy rather than
 * architecture: ESP32 wants esp_restart(), a bare-metal target a watchdog write,
 * QEMU a semihosting call. The port deliberately does not define a weak
 * fallback, so a board that forgets this hook fails to link instead of silently
 * returning from a reset.
 */
void rt_hw_board_reset(void)
{
    extern void esp_restart(void);

    esp_restart();
}

/* ---- flash / cache hooks ------------------------------------------------- */

/*
 * Strong definitions of the two hooks components/spi_flash/cache_utils.c
 * declares but only defines in its multi-core branch.
 *
 * With CONFIG_FREERTOS_UNICORE=y the single-core branch of that file calls
 * these around the window where the flash cache is off, and the weak stubs
 * live in the *other* branch - so a unicore build fails to link with
 * "undefined reference to spi_flash_other_core_stop" unless the port provides
 * them. (Measured on a plain IDF S3 project: the same two symbols, same
 * failure.)
 *
 * They are empty because there is no second core to park: this port is single
 * core and IDF never releases core 1 (CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE).
 * IRAM_ATTR because they are called with the flash cache disabled - a
 * flash-resident callee would fault there.
 */
#ifdef RT_USING_SMP
/* Handshake with the tick on the other core. Both words are plain DRAM. */
volatile rt_uint32_t xt_rt_flash_op;
volatile rt_uint32_t xt_rt_flash_ack;

/*
 * Which core opened the flash window.
 *
 * The handshake has to be answered by the OTHER core only. xPortSysTickHandler
 * runs the same code on both cores, so without this the core doing the flash
 * operation parks itself: its own tick sees op=1, acknowledges, and spins waiting
 * for op to clear - which only the code it just interrupted could do.
 *
 * Measured, before this field existed: the shell stopped after "Formatting FATFS
 * partition", no panic, no reset, and both cores were in the park loop.
 */
volatile rt_uint32_t xt_rt_flash_cpu;

/* Serialises the two cores around a flash operation. rt_hw_spin_lock/unlock are
 * IRAM-resident in libcpu/xtensa/cpuport.c, which is what makes them usable in
 * this window; the type and the declarations come from rthw.h. */
static rt_hw_spinlock_t xt_rt_flash_lock = __RT_HW_SPIN_LOCK_UNLOCKED(0);

/*
 * How deep the flash operations are nested on the core that is doing them.
 *
 * They nest: a FAT format erases many sectors and each erase goes through
 * spi_flash_disable_interrupts_caches_and_other_cpu(), which calls this pair.
 * The handshake can only run on the outermost entry - a nested stop() would
 * clear the ack that the other core has already given, and that core is sitting
 * in its park loop waiting for op to clear, so it will never ack again:
 *
 *   layer 1: op=1, ack=0  -> other core parks, acks, spins on op
 *   layer 2: op=1, ack=0  -> waits for an ack that cannot come   <- deadlock
 *
 * Measured that way: the shell stopped printing after "Formatting FATFS
 * partition" with no panic and no watchdog reset, because the parked core was
 * inside its tick handler with all interrupts masked.
 *
 * Only the doing core touches this counter, so no atomics are needed.
 */
static rt_uint32_t xt_rt_flash_depth;
#endif

void IRAM_ATTR spi_flash_other_core_stop(void)
{
#ifdef RT_USING_SMP
    rt_uint32_t spins = 0;

    /*
     * Park the other core before our instruction cache goes off.
     *
     * The empty body this replaces was written when the port was strictly single
     * core. With RT_USING_SMP the shape is different: CPU1's tick handler runs
     * from IRAM and, left unguarded, keeps fetching from flash while CPU0 has
     * the cache disabled - on Xtensa that is an instruction fetch fault.
     *
     * Measured before this change: nvs_flash_init() on CPU0 never returned. NVS
     * opens a flash operation, the erase path reaches this hook, the weak no-op
     * does nothing, and the boot stalls with the shell silent and no panic.
     *
     * The acknowledgement comes from the IRAM-resident tick handler
     * (port_systick.c), which spins until resume() clears the flag. The bounded
     * spin means a core that is not running cannot hang the boot.
     */
    /*
     * One core at a time, and only the outer layer handshakes.
     *
     * Two problems are being solved here, and they had to be separated:
     *
     * 1. NESTING on the doing core. A FAT format erases many sectors and each
     *    erase re-enters this pair. A nested stop() would reset ack to 0 while the
     *    other core is already parked and waiting for op - and that core will
     *    never acknowledge again. Measured: silent hang in the middle of a format.
     *
     * 2. TWO CORES AT ONCE. op/ack/cpu are global, so if the other core starts its
     *    own flash operation while this one is parked, it overwrites cpu with its
     *    own id and this core's tick concludes it is the doing core and stops
     *    parking - or worse, both end up in the loop waiting for each other. This
     *    is reachable in practice: Wi-Fi and BLE write NVS from their own tasks,
     *    which the scheduler may place on either core, while the shell formats.
     *    Measured as a hang that only appeared once the wireless stacks were up.
     *
     * spi_flash_op_lock() does not help: with CONFIG_FREERTOS_SMP undefined it
     * resolves to vTaskSuspendAll(), which only stops scheduling on the calling
     * core.
     *
     * So: a spinlock serialises the two cores around the whole operation, and the
     * depth counter covers re-entry on the one that holds it. The spinlock is
     * IRAM-resident (libcpu/xtensa/cpuport.c: rt_hw_spin_lock), which is what
     * makes it usable in this window.
     */
    /*
     * Depth FIRST, lock second.
     *
     * rt_hw_spin_lock() is a bare S32C1I loop and is not re-entrant: a nested
     * call on the same core finds owner == itself, the conditional store fails,
     * and it spins forever. Measured exactly that way - the shell stopped after
     * "Formatting FATFS partition" with no panic, because the first erase sector
     * nested inside the format and deadlocked on this lock.
     *
     * A nested entry is already inside the outer window, so it neither needs the
     * lock nor the handshake.
     */
    if (xt_rt_flash_depth != 0)
    {
        xt_rt_flash_depth++;
        return;
    }

    rt_hw_spin_lock(&xt_rt_flash_lock);


    xt_rt_flash_depth = 1;
    xt_rt_flash_ack = 0;
    xt_rt_flash_cpu = (rt_uint32_t)rt_hw_cpu_id();
    xt_rt_flash_op  = 1;

    /*
     * Wait for the acknowledgement WITHOUT a give-up.
     *
     * There used to be a `spins < 4000000u` bound here, added so a core that is
     * not running could not hang the boot. It is the wrong trade: 4e6 iterations
     * is roughly 16 ms, and if the other core's tick does not land in that
     * window the bound expires and this core turns the cache off anyway - with
     * the other core still free to take an interrupt. Measured exactly that way:
     *
     *   Guru Meditation Error: Core 0 panic'ed (Cache error).
     *   PC 0x4200bb2e  rt_sem_trytake                      (flash)
     *     <- ft_ipc_give_isr <- xQueueGiveFromISR
     *     <- semphr_give_from_isr_wrapper <- r_btdm_task_post_from_isr_impl
     *
     * i.e. a Bluetooth ISR on the un-parked core walked into flash while CPU1 was
     * formatting the FAT partition. The window only opened once BLE was running,
     * which is why formatting before `s3ble` never reproduced it.
     *
     * Waiting forever is safe here because the ack is produced by an IRAM tick
     * handler that cannot itself be preempted by anything this loop needs. If
     * that handler is not running, the flash operation could not have completed
     * correctly anyway - hanging is strictly better than corrupting.
     *
     * The 4e6-iteration check is kept as a console warning only, so a stuck
     * handshake is diagnosable instead of silent.
     */
    while (xt_rt_flash_ack == 0)
    {
        /* Unbounded on purpose - see the note below. The counter only decides
         * when to say something, never when to give up. */
        if (spins == 40000000u)
        {
            rt_kprintf("board: flash park handshake slow - other core has not "
                       "acknowledged yet (still waiting)\n");
            spins = 0;
        }

        spins++;
    }
#endif
}

void IRAM_ATTR spi_flash_other_core_resume(void)
{
#ifdef RT_USING_SMP
    if (xt_rt_flash_depth > 1)
    {
        /* Still inside an outer flash operation; leave the other core parked.
         * No unlock either - this level never took the lock. */
        xt_rt_flash_depth--;
        return;
    }

    xt_rt_flash_depth = 0;
    xt_rt_flash_op = 0;

    /* Release only after op is clear, so the parked core has already been told
     * it may run again. */
    rt_hw_spin_unlock(&xt_rt_flash_lock);
#endif
}

/* ---- esp_timer ----------------------------------------------------------- */

#if CONFIG_IDF_RTOS_RTTHREAD

/*
 * Start esp_timer before any SECONDARY-stage init function needs it.
 *
 * esp_timer splits in two. esp_timer_early_init() starts the counter as a
 * CORE-stage function, so esp_timer_get_time() works early. esp_timer_init() is
 * the other half - it creates the dispatch task and installs the alarm ISR - and
 * IDF reaches it from ESP_SYSTEM_INIT_FN(esp_timer_init_os, SECONDARY, ..., 100),
 * which esp_timer.c compiles out under CONFIG_IDF_RTOS_RTTHREAD with the comment
 * "RT-Thread takes over the timers".
 *
 * That assumption does not hold as soon as Wi-Fi or Bluetooth is linked in.
 * RT-Thread has its own timers, but they are not esp_timer, and IDF components
 * still call esp_timer_create(): esp_coex does it at SECONDARY priority 204
 * through ets_timer_setfn -> esp_timer_create, which returns ESP_ERR_INVALID_STATE
 * while s_timer_task is NULL. ESP_ERROR_CHECK then aborts the boot.
 *
 * Calling it from rt_hw_board_init() would be too late: board init runs from
 * entry(), after do_secondary_init() has already run every SECONDARY function.
 * Priority 99 precedes esp_timer_init_os (100, absent here), mbedtls PSA (104)
 * and coexist (204); lower numbers run first. esp_timer_init() is idempotent, so
 * a second call from anywhere is harmless.
 *
 * (Same reasoning and same priority as port/common/board.c on the RISC-V
 * targets - this is the Xtensa copy of a fix that was already needed there.)
 */
static esp_err_t rt_hw_esp_timer_init(void)
{
    extern int esp_timer_init(void);

    if (esp_timer_init() != 0)
    {
        rt_kprintf("board: esp_timer_init failed\n");
        return ESP_FAIL;
    }

    return ESP_OK;
}

ESP_SYSTEM_INIT_FN(rt_hw_esp_timer_init, SECONDARY, BIT(0), 99)
{
    return rt_hw_esp_timer_init();
}

#endif /* CONFIG_IDF_RTOS_RTTHREAD */

/* ---- deferred FreeRTOS tasks --------------------------------------------- */

#if CONFIG_IDF_RTOS_RTTHREAD_FREERTOS_SHIM

/*
 * Start the tasks that were created before the scheduler existed.
 *
 * IDF's SECONDARY-stage init functions run before rtthread_startup() reaches
 * rt_system_scheduler_init(), yet several of them create tasks: esp_timer is the
 * first (stack size 4 KB, priority 8), and anything else that pulls in a
 * component with a worker thread follows. Creating is safe that early - the
 * object and its stack exist - but starting is not, because
 * rt_thread_startup() walks rt_thread_priority_table[] and that array is only
 * initialised by rt_system_scheduler_init(). So ft_task_create() queues them and
 * this is the flush.
 *
 * Measured consequence of NOT having this on the S3 port: `list thread` showed
 *
 *   esp_timer      8  init      0x00000110  0x00001000   6%  ...
 *
 * - the task existed but had never been started, so its entry function never
 * ran, so no esp_timer callback ever fired. esp_wifi_scan_start() then returned
 * ESP_ERR_WIFI_TIMEOUT (0x300c) because the scan-complete notification is
 * delivered through an esp_timer alarm. The Wi-Fi driver itself was healthy; the
 * timer task behind it was not running.
 *
 * The RISC-V targets have the same hook in port/common/board.c. It was missing
 * here only because this port did not initialise esp_timer when it was written,
 * so nothing created a task that early.
 *
 * INIT_COMPONENT_EXPORT, not INIT_APP_EXPORT: this has to run before the
 * application stage so nothing in the app stage finds a worker thread that has
 * not started. rt_components_init() runs from main_thread_entry, i.e. after
 * rt_system_scheduler_start(), which is exactly the window needed.
 */
static int rt_hw_start_pending_tasks(void)
{
    extern void ft_task_start_pending(void);

    ft_task_start_pending();

    return 0;
}
/*
 * Register this core's IPI line, at a stage where the interrupt matrix is
 * programmable.
 *
 * rt_xt_ipi_init() allocates SYS_CPU_INTR_FROM_CPU_n for the CALLING core and
 * calls esp_intr_alloc(), which only *records* a handler until the interrupt
 * matrix is up. Called from rt_hw_board_init() it returned ESP_OK while that
 * core's INTENABLE stayed at 0x00000002 (systimer only) - measured - so the
 * line was registered but never armed and every IPI to it was dropped:
 *
 *   IC i0=0 i1=27         core 1's line works, core 0's never fires
 *   FL enter=1 sent=1 spins=17117 block=0    the flash callback never ran
 *
 * INIT_COMPONENT runs from main_thread_entry, i.e. after the matrix exists.
 * It also has to run on the core whose line is being registered, which is why
 * rt_application_init() pins `main` to core 0.
 */
#ifdef RT_USING_SMP
static int rt_hw_ipi_init(void)
{
    int rc = rt_xt_ipi_init();

    if (rc != 0)
    {
        rt_kprintf("xt-ipi: core %d registration failed\n", rt_hw_cpu_id());
    }

    return rc;
}
INIT_COMPONENT_EXPORT(rt_hw_ipi_init);
#endif

#if defined(RT_USING_SMP)
/*
 * Create the NVS mutex while this core is still the only one running.
 *
 * Under SMP the FIRST NVS lock acquisition hangs inside newlib's lazy
 * lock_init_generic(): measured `Lock::Lock() entered 1 / returned 0`,
 * `mSemaphore 0`, `lock_init_generic entries 4` (all from unrelated locks) and
 * `ft_ipc_take entries 0` - i.e. that one call never got as far as creating the
 * semaphore. Doing it here, before the scheduler, gives every later
 * Lock::Lock() a ready semaphore and skips the lazy path entirely.
 */
static int rt_hw_nvs_lock_preinit(void)
{
    extern void nvs_lock_preinit(void);
    void *probe;

    nvs_lock_preinit();

    /*
     * Same class of hazard, one layer down: esp_partition keeps its own
     * lazily-created _lock_t (partition.c:70) that guards the partition list.
     * Under SMP the first esp_partition_find_first() hung there
     * (`PL enter=1 loaded=0`), which is what nvs_flash_init() calls.
     */
    {
        extern void esp_partition_lock_preinit(void);

        esp_partition_lock_preinit();
    }

    /*
     * Prime the heap too.
     *
     * NVSPartitionManager::get_instance() does `new (std::nothrow)
     * NVSPartitionManager()` on its first call, and under SMP that `new` never
     * returns - measured `pm_in=1 lk=0/0`, i.e. the singleton accessor was
     * entered and the allocation was not. The heap's own lock is lazily created
     * on first use exactly like the NVS lock was, so doing one allocation here -
     * while this core is alone - removes the same class of hazard for malloc.
     */
    probe = rt_malloc(64);
    if (probe != RT_NULL)
    {
        rt_free(probe);
    }

    /*
     * Prime newlib's own malloc lock, which is what `new` actually takes.
     *
     * `new NVSPartitionManager()` does NOT reach pvPortMalloc - that is the
     * FreeRTOS API and the C++ runtime never calls it. Measured: pvPortMalloc's
     * entry counter stayed at 0 while the layers below it were healthy
     * (multi_heap 30/30, heap_caps 29/29). So the hang is above both, in the C
     * library: `operator new` -> malloc() -> newlib _malloc_r, whose
     * __malloc_lock is another lazily-created _lock_t - exactly the hazard that
     * was just fixed for the NVS mutex.
     */
    {
        void *p = malloc(64);

        if (p != RT_NULL)
        {
            free(p);
        }
    }

    return 0;
}
INIT_BOARD_EXPORT(rt_hw_nvs_lock_preinit);
#endif

INIT_COMPONENT_EXPORT(rt_hw_start_pending_tasks);

#endif /* CONFIG_IDF_RTOS_RTTHREAD_FREERTOS_SHIM */
