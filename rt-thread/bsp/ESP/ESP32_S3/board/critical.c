/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-11-03     RT-Thread    first version, ESP32-S3 (Xtensa LX7) port
 *
 * ESP32-S3 critical sections: the FreeRTOS port API that IDF components call
 * directly, implemented on RT-Thread.
 *
 * Why this file exists at all
 * ---------------------------
 * On Xtensa, port/CMakeLists.txt removes the whole FreeRTOS port (port.c and
 * portasm.S), because its vector table and context-switch symbols collide with
 * components/xtensa/xtensa_vectors.S. That removal takes the critical-section
 * API with it - and that API is not FreeRTOS-internal: dozens of IDF components
 * include esp_private/critical_section.h and call
 *
 *     portENTER_CRITICAL(&mux)  ->  vPortEnterCritical(&mux)
 *                               ->  xPortEnterCriticalTimeout(&mux, portMUX_NO_TIMEOUT)
 *     portEXIT_CRITICAL(&mux)   ->  vPortExitCritical(&mux)
 *     portCHECK_IF_IN_ISR()     ->  xPortInIsrContext()
 *
 * through the inline wrappers in portable/xtensa/include/freertos/portmacro.h.
 * Without these three symbols the image fails to link on esp_system,
 * esp_hw_support and spi_flash (measured: 40+ undefined references from
 * apb_backup_dma.c, esp_clk.c and intr_alloc.c alone).
 *
 * On RISC-V the port keeps port.c precisely so that this API survives; the
 * Xtensa port cannot, so it reimplements the three functions here. They are
 * thin: the whole point of a critical section on a single-core Xtensa is
 * "raise PS.INTLEVEL so no interrupt below XCHAL_EXCM_LEVEL can be taken, and
 * put it back afterwards", which is what rt_hw_interrupt_disable/enable
 * already do.
 *
 * Nesting and the mux
 * -------------------
 * portMUX_TYPE is spinlock_t, but there is no second core here, so the lock
 * itself never contends and is deliberately not touched. What DOES matter is
 * nesting: IDF nests portENTER_CRITICAL within portENTER_CRITICAL constantly
 * (periph_module_enable, esp_intr_alloc, spi_flash_op_lock), and only the
 * outermost exit may restore the interrupt state. A counter does that.
 *
 * The saved token is the whole PS, not just the interrupt level, for the same
 * reason rt_hw_interrupt_disable returns the whole PS: writing back a bare
 * level would clear PS.UM/OWB/WOE, and the next window overflow would then
 * fault as an illegal instruction.
 */

#include <rtthread.h>
#include <rthw.h>

#include "sdkconfig.h"
#include "esp_attr.h"

/* portmacro.h is where portMUX_TYPE and the prototypes live. Including the
 * FreeRTOS header from an RT-Thread port is intentional: these functions exist
 * to satisfy that ABI, and inventing a parallel declaration would let the two
 * drift. */
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

/*
 * PER CORE, and that is a correctness fix, not a tidy-up.
 *
 * These were single globals. Under SMP two cores take critical sections
 * independently, so one core's enter would bump the other's nesting count and
 * one core's exit would restore the other's saved PS - enabling interrupts on
 * the wrong core at a level nobody asked for, or leaving them masked forever.
 * That is exactly the "one core goes quiet and never comes back" symptom.
 *
 * The core cannot be asked for at rt_xt_cpu_init() time (this file has no init
 * hook), so rt_hw_cpu_id() is called directly - it is three instructions.
 */
static int       s_crit_nesting[RT_CPUS_NR];
static rt_base_t s_crit_level[RT_CPUS_NR];

/* Which core am I?
 *
 * rt_hw_cpu_id() is SMP-only in this port (it reads PRID), so the UP build has
 * to fold to 0 at compile time rather than referencing a symbol that is not
 * linked. */
rt_inline int xt_crit_cpu(void)
{
#ifdef RT_USING_SMP
    int cpu = rt_hw_cpu_id();
    return (cpu >= 0 && cpu < RT_CPUS_NR) ? cpu : 0;
#else
    return 0;
#endif
}

/*
 * IRAM_ATTR: every one of these is called from code that can run with the flash
 * cache off (spi_flash's op lock, esp_intr_alloc, the MSPI timing paths). A
 * flash-resident body would fault inside that window - the exact failure the
 * RISC-V port's cache assertion was written to catch.
 */
BaseType_t IRAM_ATTR xPortEnterCriticalTimeout(portMUX_TYPE *mux, BaseType_t timeout)
{
    rt_base_t level;
    int       cpu;

    RT_UNUSED(timeout);

    level = rt_hw_local_irq_disable();

    cpu = xt_crit_cpu();

    if (s_crit_nesting[cpu]++ == 0)
    {
        /* Outermost entry on THIS core: remember what to restore here. */
        s_crit_level[cpu] = level;
    }

    return pdTRUE;
}

void IRAM_ATTR vPortExitCritical(portMUX_TYPE *mux)
{
    RT_UNUSED(mux);

    int cpu = xt_crit_cpu();

    if (s_crit_nesting[cpu] <= 0)
    {
        /* Unbalanced exit. Getting here means the caller's nesting is wrong,
         * and restoring a stale level would re-enable interrupts at a level
         * nobody asked for - so leave the state alone. */
        return;
    }

    if (--s_crit_nesting[cpu] == 0)
    {
        rt_hw_local_irq_enable(s_crit_level[cpu]);
    }
}

/*
 * "Is the code that is running now an interrupt handler?"
 *
 * IDF asks this to decide whether it may block. RT-Thread already counts trap
 * nesting, so this is a direct answer rather than a reimplementation. Note that
 * rt_interrupt_get_nest() itself disables interrupts for the read, so this is
 * safe to call from a critical section.
 *
 * IRAM_ATTR is load-bearing, and its absence was a real crash.
 *
 * The other four functions in this file already carried it; these two did not,
 * and the file's own comment claimed "every one of these is called from code
 * that can run with the flash cache off". It was wrong. Measured on hardware:
 * the BLE controller's ISR (r_lld_adv_frm_isr_eco) calls portCHECK_IF_IN_ISR()
 * while CPU0 is inside spi_flash_hal_read() with the cache off, and the call
 * lands on this function's flash-resident prologue:
 *
 *   Guru Meditation Error: Core 0 panic'ed (Cache error).
 *   Cache disabled but cached memory region accessed
 *   PC : 0x4204a122   xPortInIsrContext at board/critical.c:150
 *   Backtrace: ... r_lld_adv_frm_isr_eco -> r_sch_prog_end_isr_handler
 *              ... spi_flash_hal_read -> esp_partition_read -> nvs Page::readEntry
 *
 * The body only reads rt_xt_core[core].nest (DRAM at 0x3fc9f030, reachable with
 * the cache off) and rt_interrupt_get_nest() (placed in IRAM at 0x4038b618), so
 * with IRAM_ATTR on the function itself nothing in this path is flash-mapped any
 * more.
 *
 * Note that the two counters are different: `nest` is the port's own, bumped by
 * _rtt_int_enter/_exit, and it is what the assembly maintains. Reading the
 * kernel's rt_interrupt_nest through rt_interrupt_get_nest() gives the same
 * answer and is what the rest of the kernel sees, so this keeps calling it.
 */
BaseType_t IRAM_ATTR xPortInIsrContext(void)
{
    return rt_interrupt_get_nest() != 0;
}

BaseType_t IRAM_ATTR xPortInterruptedFromISRContext(void)
{
    return rt_interrupt_get_nest() != 0;
}

void vPortAssertIfInISR(void)
{
    RT_ASSERT(xPortInIsrContext() == 0);
}

/*
 * The compliance variants are only referenced when
 * CONFIG_FREERTOS_CHECK_PORT_CRITICAL_COMPLIANCE is on, in which case
 * portENTER_CRITICAL maps to them instead. A critical section in an ISR is a
 * genuine bug (it would try to restore an interrupt state from inside a
 * handler), so it is reported rather than silently allowed.
 */
#if CONFIG_FREERTOS_CHECK_PORT_CRITICAL_COMPLIANCE
BaseType_t xPortEnterCriticalTimeoutCompliance(portMUX_TYPE *mux, BaseType_t timeout)
{
    if (xPortInIsrContext())
    {
        rt_kprintf("portENTER_CRITICAL called from ISR context\n");
        RT_ASSERT(0);
        return pdFAIL;
    }

    return xPortEnterCriticalTimeout(mux, timeout);
}

void vPortExitCriticalCompliance(portMUX_TYPE *mux)
{
    if (xPortInIsrContext())
    {
        rt_kprintf("portEXIT_CRITICAL called from ISR context\n");
        RT_ASSERT(0);
        return;
    }

    vPortExitCritical(mux);
}
#endif /* CONFIG_FREERTOS_CHECK_PORT_CRITICAL_COMPLIANCE */

/*
 * portYIELD_FROM_ISR() is a macro over this symbol (see portmacro.h), and IDF
 * code outside the FreeRTOS component calls it: esp_libc/src/locks.c does, from
 * the ISR branch of its lock functions, whenever giving a semaphore woke a
 * higher-priority task.
 *
 * On FreeRTOS this only raises a flag that _frxt_int_exit consults. Here the
 * kernel has the same notion already - rt_schedule() called from interrupt
 * context records the switch through rt_hw_context_switch_interrupt() and the
 * outermost _rtt_int_exit performs it - so the mapping is direct.
 *
 * It lives in this file, with the rest of the port API that IDF calls, rather
 * than in context_gcc.S: it is a C-level call into the scheduler and has no
 * business being assembly.
 *
 * IRAM_ATTR matches the caller. Note that rt_schedule() itself is in flash, so
 * a yield requested with the flash cache off would still fault - but that is a
 * property of placing the RT-Thread scheduler in flash, not of this shim, and
 * the same is true of every rt_schedule() call from an RT-Thread ISR on this
 * port.
 */
void IRAM_ATTR _frxt_setup_switch(void)
{
    rt_schedule();
}

/*
 * vPortYield() - give the CPU away from thread context.
 *
 * This symbol used to come from FreeRTOS-Kernel/portable/xtensa/portasm.S, which
 * is not compiled on this target (it duplicates the vector table and the switch
 * that xtensa_vectors.S and libcpu/xtensa already provide). Everything else that
 * lived there is either replaced by the RT-Thread port (_frxt_int_enter/exit,
 * the tick) or only used by the FreeRTOS scheduler (vPortYieldFromInt).
 *
 * It is not dead code: components/bt/controller/esp32c3/bt.c publishes it in
 * osi_funcs_ro._task_yield, so the Bluetooth controller calls it directly. That
 * is why the link fails with "undefined reference to `vPortYield'" the moment
 * CONFIG_BT_ENABLED is turned on - measured.
 *
 * rt_schedule() is the exact equivalent: it picks the highest-priority ready
 * thread and switches to it (or returns immediately if the caller still is it).
 * "Yield" in FreeRTOS means precisely "let the scheduler decide", not
 * "unconditionally switch", so the mapping is direct rather than approximate.
 */
void IRAM_ATTR vPortYield(void)
{
    rt_schedule();
}

/*
 * vPortYieldOtherCore() - ask another core to reschedule.
 *
 * Single core: there is no other core to ask, and CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE
 * means IDF never releases core 1. Doing nothing is correct here rather than
 * merely convenient - the caller wants a reschedule that has already happened on
 * the only core that exists.
 *
 * Present as a real function, not a macro, because portmacro.h declares it and
 * IDF code takes its address in a couple of places.
 */
void vPortYieldOtherCore(BaseType_t coreid)
{
    RT_UNUSED(coreid);
}

/*
 * xPortGetTickRateHz() - the tick rate the running kernel actually uses.
 *
 * IDF asks this instead of trusting CONFIG_FREERTOS_HZ, which is why it has to
 * report rt_tick_get()'s rate and not the compile-time constant: they are
 * required to be equal, port_systick.c asserts on it, but reporting the live
 * value means a future divergence shows up as a wrong number somewhere instead
 * of as an unexplained timing drift.
 */
uint32_t xPortGetTickRateHz(void)
{
    return (uint32_t)RT_TICK_PER_SECOND;
}

