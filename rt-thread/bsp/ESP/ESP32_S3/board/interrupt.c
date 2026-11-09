/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-11-03     RT-Thread    first version, ESP32-S3 (Xtensa LX7) port
 *
 * ESP32-S3 interrupt bridge between RT-Thread and the ESP-IDF interrupt
 * matrix, for the Xtensa port.
 *
 * This file is much smaller than port/esp32s31/interrupt.c, and the reason is
 * architectural rather than a shortcut. On RISC-V the port owns the trap entry,
 * so it also owns the vector table, the per-line handler slots and the CLIC
 * level programming - three things that had to be written from scratch. On
 * Xtensa all of that already exists in ESP-IDF and is shared with FreeRTOS:
 *
 *   components/xtensa/xtensa_vectors.S    _xt_lowint1 -> dispatch_c_isr
 *   components/xtensa/xtensa_intr.c       xt_set_interrupt_handler()
 *   components/esp_hw_support/intr_alloc.c  esp_intr_alloc()
 *
 * dispatch_c_isr() walks `_xt_interrupt_table`, which xt_set_interrupt_handler()
 * fills. So "install an RT-Thread ISR" is exactly "fill a slot in that table",
 * and there is no second dispatcher to keep in sync.
 *
 * Numbering. RT-Thread's `vector` here is the **CPU interrupt number**
 * (`XCHAL_*_INUM`, 0..XCHAL_NUM_INTERRUPTS-1), i.e. the same number
 * xt_set_interrupt_handler() wants and the same one esp_intr_alloc() reports in
 * `esp_intr_handle_t`. It is not the peripheral's `ETS_*_INTR_SOURCE`; a source
 * has to be routed onto a CPU line first (`esp_rom_route_intr_matrix`), and
 * IDF's allocator does that itself.
 *
 * Level. Xtensa has a fixed interrupt level per line, described by
 * `Xthal_intlevel[]` and baked into the vectors. There is nothing to program -
 * which is why this file has no equivalent of the S31 port's
 * rt_hw_interrupt_set_priority(). xt_set_interrupt_handler() refuses any line
 * above XCHAL_EXCM_LEVEL, because those vectors do not obey the C ABI and must
 * be written in assembly. That refusal is the correct answer, not an error to
 * work around.
 */

#include <rthw.h>
#include <rtthread.h>

#include "sdkconfig.h"
#include "esp_attr.h"
#include "esp_rom_sys.h"

#include <xtensa/config/core.h>
#include <xtensa_api.h>

#include "port_intr.h"

/*
 * RT-Thread ISRs take (int vector, void *param); Xtensa's take (void *arg).
 * A cast would hand the handler its own parameter as the vector, so every
 * registration goes through a trampoline whose argument is a small descriptor.
 */
struct rtt_isr_ctx
{
    rt_isr_handler_t fn;
    void            *param;
    int              vector;
};

static struct rtt_isr_ctx s_ctx[XCHAL_NUM_INTERRUPTS];

/*
 * The one function with the signature xt_handler expects.
 *
 * IRAM_ATTR because a level-1 line can fire while the flash cache is off (the
 * window IDF opens for SPI-flash writes and MSPI timing tuning). A
 * flash-resident trampoline would fault there, and the usual symptom is a
 * crash inside a flash write with no message.
 */
static void IRAM_ATTR rtt_trampoline(void *arg)
{
    struct rtt_isr_ctx *ctx = (struct rtt_isr_ctx *)arg;

    ctx->fn(ctx->vector, ctx->param);
}

rt_isr_handler_t rt_hw_interrupt_install(int vector, rt_isr_handler_t handler,
                                         void *param, const char *name)
{
    rt_isr_handler_t previous = RT_NULL;

    RT_UNUSED(name);

    if (vector < 0 || vector >= XCHAL_NUM_INTERRUPTS)
    {
        return RT_NULL;
    }

    /*
     * The level check, done here rather than inferred from the return value.
     *
     * xt_set_interrupt_handler() refuses a line above XCHAL_EXCM_LEVEL because
     * those vectors do not obey the C ABI and must be written in assembly. It
     * signals that by returning 0 - and it returns 0 for the *normal* case too,
     * namely "there was no previous handler". So its return value cannot be
     * used to tell success from failure, and an earlier version of this file
     * did exactly that: it treated the successful install of the UART line as a
     * rejection, cleared the slot, and the trampoline jumped to NULL at the
     * first character typed (measured: InstrFetchProhibited, PC 0x00000000).
     *
     * Hence: check the level directly against Xthal_intlevel[], install
     * unconditionally, and report only the case that is genuinely refused.
     */
    if (Xthal_intlevel[vector] > XCHAL_EXCM_LEVEL)
    {
        rt_kprintf("intr: line %d needs an assembly handler (level %d > EXCM %d)\n",
                   vector, (int)Xthal_intlevel[vector], (int)XCHAL_EXCM_LEVEL);
        return RT_NULL;
    }

    previous = s_ctx[vector].fn;

    s_ctx[vector].fn     = handler;
    s_ctx[vector].param  = param;
    s_ctx[vector].vector = vector;

    /* Fills _xt_interrupt_table, which is the table dispatch_c_isr walks. */
    xt_set_interrupt_handler(vector, rtt_trampoline, &s_ctx[vector]);

    return previous;
}

void rt_hw_interrupt_uninstall(int vector, rt_isr_handler_t handler, void *param)
{
    if (vector < 0 || vector >= XCHAL_NUM_INTERRUPTS)
    {
        return;
    }

    /* Only clear the slot when it still belongs to this caller, so an
     * unrelated owner of the same line is left alone. */
    if (s_ctx[vector].fn == handler && s_ctx[vector].param == param)
    {
        s_ctx[vector].fn = RT_NULL;
        xt_set_interrupt_handler(vector, RT_NULL, RT_NULL);
    }
}

void rt_hw_interrupt_mask(int vector)
{
    if (vector < 0 || vector >= XCHAL_NUM_INTERRUPTS)
    {
        return;
    }

    /* INTCLEAR first: masking a line that is already latched leaves the
     * pending bit set, and Xtensa re-raises it as soon as the mask is lifted
     * - which looks like an interrupt storm on the next driver that enables
     * the line. */
    xt_ints_off(1u << vector);
}

void rt_hw_interrupt_umask(int vector)
{
    if (vector < 0 || vector >= XCHAL_NUM_INTERRUPTS)
    {
        return;
    }

    xt_ints_on(1u << vector);
}

rt_bool_t rt_hw_interrupt_is_masked(int vector)
{
    rt_uint32_t enable;

    if (vector < 0 || vector >= XCHAL_NUM_INTERRUPTS)
    {
        return RT_TRUE;
    }

    __asm__ volatile ("rsr %0, intenable" : "=r"(enable));

    return (enable & (1u << vector)) == 0;
}

/*
 * Route a peripheral source onto a CPU line.
 *
 * Every INTERRUPT_CORE0_*_MAP register resets to 0, so out of reset the whole
 * matrix points at CPU line 0. Installing a handler without routing the source
 * does not fail - the handler is simply never reached, and the peripheral looks
 * dead. IDF's esp_intr_alloc() does this step itself; this wrapper is for the
 * drivers that install directly through rt_hw_interrupt_install().
 */
int rt_hw_intr_route(int source, int vector)
{
    if (source < 0 || vector < 0 || vector >= XCHAL_NUM_INTERRUPTS)
    {
        return -RT_EINVAL;
    }

    esp_rom_route_intr_matrix(0, (uint32_t)source, (uint32_t)vector);

    return 0;
}
