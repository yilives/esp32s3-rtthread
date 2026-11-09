#include <rtthread.h>
#include "sdkconfig.h"
#include "port_systick.h"
#include "riscv/interrupt.h"
#include "esp_heap_caps_init.h"
#include "esp_err.h"
#include "esp_private/startup_internal.h"

/* rt_hw_intr_mask_unhandled(). Only the S31 port defines it, so it is declared
 * locally rather than pulling in a target-specific header from the common
 * board file. */
extern int rt_hw_intr_mask_unhandled(void);

void rt_hw_board_init(void)
{
    /* startup.c:do_core_init() already initialized the RT-Thread heap.
     * Reinitializing it here would erase and reinsert its memory object.
     */

    /* rtthread_startup() has disabled MIE. Reuse IDF's OS tick alarm; the
     * first thread's mret enables interrupts after timer/scheduler init.
     * vectors_rtthread.S saves, raises and restores the interrupt threshold in
     * software on both C3 and C6, which do not define
     * SOC_INT_HW_NESTED_SUPPORTED.
     */
    vPortSetupTimer();
    esprv_int_set_threshold(RVHAL_INTR_ENABLE_THRESH);

#if defined(SOC_ESP32_S31)
    /* esp_timer has to be finished here, and the reason is a gap between two
     * decisions that were each correct on their own.
     *
     * esp_timer splits into two halves. esp_timer_early_init() only starts the
     * SYSTIMER counter, and it runs as a CORE-stage init function, so
     * esp_timer_get_time() already works. esp_timer_init() is the other half:
     * it creates the dispatch task and installs the alarm ISR, and it is reached
     * from ESP_SYSTEM_INIT_FN(esp_timer_init_os) - which IDF compiles out under
     * CONFIG_IDF_RTOS_RTTHREAD, because upstream assumes the RTOS brings its own
     * timers.
     *
     * RT-Thread does have its own timers, but they are not esp_timer, and
     * nothing in this port calls esp_timer_init(). The result is a driver that
     * links, runs, and then aborts on first use: esp_phy_enable() calls
     * phy_track_pll_init() unconditionally, that calls
     * ESP_ERROR_CHECK(esp_timer_create(...)), and esp_timer_create() returns
     * ESP_ERR_INVALID_STATE whenever s_timer_task is still NULL
     * (esp_timer.c:105). An ESP_ERROR_CHECK on a non-OK code aborts.
     *
     * So the missing half is supplied here. Called before the scheduler starts,
     * which is fine: rt_thread_startup() only puts the thread on the ready
     * queue. It is also safe to call once - init_timer_task() returns
     * ESP_ERR_INVALID_STATE if it has already run.
     *
     * Gated on the shim, because esp_timer's task is only needed by the IDF
     * components that run on it (esp_phy, esp_wifi). With the shim off nothing
     * in the image calls esp_timer_create(), so the task would be dead weight. */
    /* esp_timer_init() moved to a SECONDARY-stage init function; see below. */

    /* Register the pin device before the scheduler runs so anything that uses
     * rt_pin_*() during init finds it.
     *
     * This used to be a hand call here, with a comment claiming it was also the
     * only reference keeping the GPIO object in the archive. That was a workaround
     * for a missing INIT_BOARD_EXPORT, not a requirement: gpio.c now exports
     * rt_hw_pin_init through the board band and rt_components_board_init() below
     * runs it. Measured in `list device` - the pin device is still there, so the
     * standard mechanism works on this port and the other drivers can follow. */
    extern int s31_selftest_link_anchor(void);
    extern int s31_board_extra_link_anchor(void);
#ifdef RT_USING_SMP
extern int s31_smp_cmd_link_anchor(void);
    extern void s31_con_cmd_link_anchor(void);
#endif
    extern int s31_intr_debug_link_anchor(void);
    extern int rt_hw_pwm_init(void);
    extern int rt_hw_timer_init(void);
    extern int rt_hw_int_wdt_init(void);
    extern int rt_hw_i2c_init(void);
    extern int rt_hw_spi_init(void);
    extern int rt_hw_adc_init(void);
    extern int rt_hw_pcnt_init(void);
    extern int rt_hw_mcpwm_init(void);
    extern int rt_hw_rmt_init(void);
    extern int rt_hw_twai_init(void);
    extern int s31_twai_cmd_link_anchor(void);

    /* The interrupt watchdog, which the FreeRTOS path starts inside
     * esp_startup_start_app() and this port therefore does not get. See
     * port/esp32s31/boot_tasks.c for the full list of what that branch skips
     * and why the task watchdog is deliberately not among them. */
    rt_hw_int_wdt_init();

    /* Fill the per-CPU interrupt-nest table and then check it.
     *
     * The table is in .bss, so it is all-NULL until this runs, and the trap handler
     * dereferences it unconditionally - a NULL slot is a load access fault at
     * address 0. What keeps that from happening today is not this call but
     * rtthread_startup()'s opening rt_hw_local_irq_disable(): MIE is clear, so no
     * trap can be taken before board init. That is a contract with the caller, and it
     * is invisible at the point where it matters, so the check below turns the
     * assumption into a message.
     *
     * The old comment claimed the reason was that the MP scheduler did not exist yet.
     * It does: _cpus is in .bss and addressable from the first instruction. The
     * comment described a belief, not the mechanism.
     *
     * See port/esp32s31/smp_stack.c for the full account. */
    {
        extern rt_err_t rt_hw_irq_nest_init(void);
        extern rt_err_t rt_hw_irq_nest_check(void);

        rt_hw_irq_nest_init();

        if (rt_hw_irq_nest_check() != RT_EOK)
        {
            rt_kprintf("board: interrupt nest table is broken, traps will fault\n");
        }
    }

    /* The pin device, the PWM/timer devices and the four bus drivers all register
     * themselves through INIT_BOARD_EXPORT now, and the board band runs at the end
     * of this function - before the scheduler, which is all any of them needs.
     * The hand calls that used to sit here were a workaround for the exports not
     * being linked in; each one is now backed by a `-u` in CMakeLists.txt, which is
     * the piece ESP-IDF's per-component archives actually require. */
    /*
     * Link anchors, not initialisation.
     *
     * These five `*_link_anchor()` calls do nothing at run time. They exist
     * because the objects they name register their msh commands from a data
     * section (FSymTab), and --gc-sections cannot see a data-section reference -
     * so without a call from somewhere the whole member is dropped and the
     * commands silently do not exist. The functions are deliberately empty, and
     * deliberately named `link_anchor`: calling them `_init` made a previous
     * reader hunt for initialisation logic that was never there.
     *
     *   s31_twai_cmd     - s31twai
     *   s31_selftest     - s31test, plus the compile-time tick check
     *   s31_board_extra  - s31psram, s31led
     *   s31_smp_cmd      - s31smp
     *   s31_intr_debug   - s31intr
     */
    s31_twai_cmd_link_anchor();
    s31_selftest_link_anchor();
    s31_board_extra_link_anchor();
#ifdef RT_USING_SMP
    s31_smp_cmd_link_anchor();
    s31_con_cmd_link_anchor();
#endif
    s31_intr_debug_link_anchor();
#endif

    /*
     * Run the BOARD stage of the auto-init table.
     *
     * This call is what keeps the .rti_fn.0 .. .rti_fn.1.end band alive. It was
     * missing, and the consequence was not a bug today but a trap for tomorrow:
     * nothing in this image uses INIT_BOARD_EXPORT(), rt_components_board_init()
     * was never referenced, --gc-sections dropped it, and a later
     * INIT_BOARD_EXPORT(fn) would have linked cleanly and then silently never
     * run - the table entry exists either way, only the walker is gone.
     *
     * Order is the RT-Thread convention: last thing in board init, after the
     * clock/tick/pin setup above that a board-stage user would depend on.
     * rt_components_init() (the .1.end .. .6.end band) is called separately from
     * main_thread_entry, so the two halves stay in their documented stages.
     */
#ifdef RT_USING_COMPONENTS_INIT
    rt_components_board_init();
#endif

    /*
     * Last, and deliberately so: mask every CPU interrupt line that is enabled
     * but has no handler.
     *
     * This is the fix for the storm that made PSRAM unusable, and it has to run
     * here rather than earlier because the SECONDARY-stage IDF init functions
     * (esp_timer, esp_wifi, the BT controller, and - with CONFIG_SPIRAM - the
     * MSPI/PSRAM setup) allocate and enable their lines after rt_hw_board_init()
     * begins. `s31intr` shows the result: lines 16/18/19/21 (LP_ADC,
     * AHB_PDMA_IN_CH0, SYSTIMER_TARGET1, UART1) arrive already enabled with an
     * empty handler table even with PSRAM off.
     *
     * A level-triggered line in that state never reaches our trampoline at all -
     * _global_interrupt_handler() returns when the slot is NULL - so the trap
     * re-enters forever and only the interrupt watchdog reports it. Masking
     * loses nothing (there is no handler) and any driver that wants the line
     * later re-enables it through rt_hw_interrupt_install().
     */
    {
        int masked = rt_hw_intr_mask_unhandled();

        if (masked > 0)
        {
            rt_kprintf("intr: masked %d enabled line(s) with no handler\n", masked);
        }
    }
}

/*
 * Give back the DRAM that IDF holds for the boot-time stack.
 *
 * soc_memory_regions (heap/port/<target>/memory_layout.c) fences off
 * [APP_USABLE_DRAM_END, SOC_DIRAM_DRAM_HIGH) as startup_stack=true, and
 * heap_caps_init deliberately leaves such a region unregistered with the
 * comment "Will be registered when OS scheduler starts". On the FreeRTOS path
 * that promise is kept by main_task -> reclaim_startup_stack_memory_for_heap()
 * -> heap_caps_enable_nonos_stack_heaps().
 *
 * startup.c:257 skips esp_startup_start_app() under CONFIG_IDF_RTOS_RTTHREAD, so
 * main_task never runs and nobody keeps it. On S31 that region is
 * 0x2f07afb0..0x2f080000, exactly 20 KB - measured, not estimated: `nm` shows
 * __heap_end__ at 0x2f045580 and the region ends at 0x2f080000, so the gap
 * between them is 20 KB larger than the registered heaps account for. With
 * Wi-Fi up (10 static RX buffers = 16 KB before anything else) that is not a
 * rounding error.
 *
 * Timing is the part that has to be right, and it is why this is an
 * INIT_APP_EXPORT rather than another line in rt_hw_board_init(): the region
 * contains SOC_ROM_STACK, and rt_hw_board_init() runs on that very stack. The
 * application-init stage runs from rt_components_init(), inside the main
 * thread, after the scheduler has switched to a stack the linker allocated - the
 * same point main_task occupies on the FreeRTOS path.
 */
static int rt_hw_reclaim_startup_stack(void)
{
    heap_caps_enable_nonos_stack_heaps();

    return 0;
}
INIT_APP_EXPORT(rt_hw_reclaim_startup_stack);

#if CONFIG_IDF_RTOS_RTTHREAD

/*
 * Start esp_timer before any SECONDARY-stage init function needs it.
 *
 * esp_timer splits in two. esp_timer_early_init() starts the counter as a
 * CORE-stage function, so esp_timer_get_time() works early. esp_timer_init()
 * is the other half - it creates the dispatch task and installs the alarm ISR -
 * and IDF reaches it from ESP_SYSTEM_INIT_FN(esp_timer_init_os, SECONDARY, ...,
 * 100), which esp_timer.c:660 compiles out under CONFIG_IDF_RTOS_RTTHREAD with
 * the comment "RT-Thread takes over the timers".
 *
 * That assumption does not hold here. RT-Thread has its own timers, but they are
 * not esp_timer, and IDF components still call esp_timer_create(): esp_coex does
 * it at SECONDARY priority 204 through ets_timer_setfn -> esp_timer_create, which
 * returns ESP_ERR_INVALID_STATE while s_timer_task is NULL (esp_timer.c:105).
 * ESP_ERROR_CHECK then aborts the boot at ets_timer_legacy.c:58.
 *
 * Calling it from rt_hw_board_init() instead - the first version of this fix -
 * is too late: board init runs from entry(), after do_secondary_init() has
 * already run every SECONDARY function. It only looked fine because the image
 * used to panic earlier, in do_core_init, before ever reaching coexist.
 *
 * Priority 99 precedes esp_timer_init_os (100, absent here), mbedtls PSA (104)
 * and coexist (204); lower numbers run first. esp_timer_init() is idempotent,
 * so a second call from anywhere is harmless.
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

#if CONFIG_IDF_RTOS_RTTHREAD_FREERTOS_SHIM

/*
 * Start the tasks that were created before the scheduler existed.
 *
 * IDF's SECONDARY-stage init functions run before rtthread_startup() reaches
 * rt_system_scheduler_init(), yet several of them create tasks: esp_timer is the
 * first, and anything else that pulls in a component with a worker thread
 * follows. Creating is safe that early, starting is not - see
 * ft_task_start_pending().
 *
 * INIT_COMPONENT_EXPORT, not INIT_APP_EXPORT: this has to run before the
 * application stage so that nothing in the app stage finds a worker thread that
 * has not started. rt_components_init() runs from main_thread_entry, i.e. after
 * rt_system_scheduler_start(), which is exactly the window needed.
 */
static int rt_hw_start_pending_tasks(void)
{
    extern void ft_task_start_pending(void);

    ft_task_start_pending();

    return 0;
}
INIT_COMPONENT_EXPORT(rt_hw_start_pending_tasks);

/*
 * Sweep unhandled interrupt lines again, once the app stage has run.
 *
 * The sweep at the end of rt_hw_board_init() is not enough on its own: measured
 * on this board it masks 3 lines, and `s31intr` afterwards still lists 4
 * (16/18/19/21 = LP_ADC, AHB_PDMA_IN_CH0, SYSTIMER_TARGET1, UART1). Those get
 * their IE bit set by something that runs after board init - the ROM leaves some
 * of them on, and the IDF drivers that own them install handlers through
 * esp_intr_alloc() later still.
 *
 * Repeating the sweep here closes that window without needing to know which of
 * the two it is. Lines that were legitimately installed by now have a non-NULL
 * handler in the IDF table and are left alone; only the genuinely orphaned ones
 * are masked. This is the difference between "PSRAM crashes the board" and
 * "PSRAM works", so it is worth the second pass.
 */
static int rt_hw_intr_sweep_again(void)
{
    extern int rt_hw_intr_mask_unhandled(void);

    int masked = rt_hw_intr_mask_unhandled();

    if (masked > 0)
    {
        rt_kprintf("intr: masked %d more line(s) after app init\n", masked);
    }

    return 0;
}
INIT_APP_EXPORT(rt_hw_intr_sweep_again);

#endif /* CONFIG_IDF_RTOS_RTTHREAD_FREERTOS_SHIM */
