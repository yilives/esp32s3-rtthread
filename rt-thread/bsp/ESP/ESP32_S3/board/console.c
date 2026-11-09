/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-11-03     RT-Thread    first version, ESP32-S3 (Xtensa LX7) port
 *
 * ESP32-S3 console: UART0, ROM output plus interrupt-driven input.
 *
 * Not an rt_serial device. A bring-up image needs one thing from its console -
 * that rt_kprintf and msh work - and both the ROM writer and this read path
 * give that with no device object, no ring buffer framework and no dependency
 * on components/esp_driver_uart (which calls FreeRTOS blocking APIs).
 * RT_USING_DEVICE is off in rtconfig.h for the same reason, and kservice.c's
 * _kputs() falls back to rt_hw_console_output() when no console device is
 * registered - which is exactly this function.
 *
 * Input is interrupt-driven on purpose
 * ------------------------------------
 * A polling reader would work, and it is what the first version of this file
 * did. It left the port's interrupt API completely untested, though:
 * rt_hw_interrupt_install(), the trampoline, xt_set_interrupt_handler() and the
 * INTENABLE mask/unmask pair could all have been broken and nothing in the image
 * would have noticed. Typing one character now exercises the whole chain:
 *
 *     UART0 RX FIFO -> ETS_UART0_INTR_SOURCE -> matrix -> CPU line
 *       -> _Level1Vector -> _xt_lowint1 -> _rtt_int_enter -> dispatch_c_isr
 *       -> rtt_trampoline -> s3_uart_isr
 *
 * The polling read is kept as the fallback in rt_hw_console_getchar(), so if the
 * interrupt never arrives the console still works - it just never fills the ring
 * and `s3rt irq` reports zero. That is deliberately a soft failure with a
 * visible symptom rather than a console that stops responding.
 */

#include <rtthread.h>

#include "sdkconfig.h"
#include "soc/uart_struct.h"
#include "soc/interrupts.h"
#include "hal/uart_ll.h"
#include "esp_rom_serial_output.h"

#include "port_intr.h"

/* The ROM console is on UART0 at CONFIG_ESP_CONSOLE_UART_BAUDRATE; this port
 * never reconfigures the rate or the frame format. */
#define RT_S3_CONSOLE_UART      UART0

/* Depth of the ISR-to-shell ring. 128 bytes is far more than a typed line and
 * costs nothing; a pasted command line that overflows it is reported by the
 * overrun counter rather than silently truncated. */
#define RT_S3_CON_RING_SIZE     128

/* RX timeout in bit times. Short enough that a single typed character raises
 * RXFIFO_TOUT instead of waiting for the FIFO to fill. */
#define RT_S3_CON_RX_TOUT       10

#define RT_S3_CON_RX_MASK       (UART_RXFIFO_FULL_INT_RAW | UART_RXFIFO_TOUT_INT_RAW)

static rt_uint8_t  s_ring[RT_S3_CON_RING_SIZE];
static rt_uint16_t s_ring_head;
static rt_uint16_t s_ring_tail;

/* Diagnostics, printed by `s3rt irq`. volatile because the ISR writes them and
 * the shell thread reads them with no lock. */
static volatile rt_uint32_t s_isr_count;
static volatile rt_uint32_t s_isr_overruns;
static rt_bool_t            s_uart_ready;

/* ---- output -------------------------------------------------------------- */

/*
 * Console output.
 *
 * esp_rom_output_putc() writes through the ROM's own UART state, which is the
 * only writer guaranteed to work before this port has initialised anything and
 * after it has crashed. It lives in ROM, so it also works with the flash cache
 * disabled - rt_kprintf from a panic path included.
 *
 * The '\n' -> "\r\n" translation is done here because the ROM writer emits the
 * byte it is given and nothing else.
 */
void rt_hw_console_output(const char *str)
{
    while (*str)
    {
        if (*str == '\n')
        {
            esp_rom_output_putc('\r');
        }
        esp_rom_output_putc(*str++);
    }
}

/* ---- input --------------------------------------------------------------- */

/*
 * Weak hook the serial device registers with.
 *
 * The RX interrupt has exactly one owner - this file - because two handlers on
 * one line would fight over the FIFO and the interrupt status register. But
 * dev_serial.c's INT_RX path does not poll: it blocks the shell thread on
 * rx_sem and only wakes when rt_hw_serial_isr() is called. So after filling the
 * ring the ISR has to tell the device layer, and the device layer is not always
 * present (the console works before serial.c registers uart0, and on a build
 * without RT_USING_SERIAL).
 *
 * A weak function is the seam: serial.c overrides it, everything else gets the
 * empty default and the ring remains the only consumer.
 */
rt_weak void rt_hw_console_rx_notify(void)
{
}

/*
 * IRAM_ATTR: a level-1 line can fire while the flash cache is off (the window
 * IDF opens for SPI-flash writes and MSPI timing tuning), and a flash-resident
 * ISR would fault there. Everything it calls is a static-inline LL helper, so
 * the whole body stays in IRAM.
 */
static void IRAM_ATTR s3_uart_isr(int vector, void *param)
{
    RT_UNUSED(vector);
    RT_UNUSED(param);

    s_isr_count++;

    /* The MASKED status, not int_raw: int_raw acknowledges every latched bit
     * including the error flags, which would then be cleared and never
     * reported. int_st is raw AND the enable mask, so only what this driver
     * asked for is consumed. */
    rt_uint32_t status = uart_ll_get_intsts_mask(&RT_S3_CONSOLE_UART);

    uart_ll_clr_intsts_mask(&RT_S3_CONSOLE_UART, status);

    if (status & UART_RXFIFO_OVF_INT_RAW)
    {
        s_isr_overruns++;
    }

    if (status & RT_S3_CON_RX_MASK)
    {
        uint8_t byte;
        rt_bool_t got = RT_FALSE;

        while (uart_ll_get_rxfifo_len(&RT_S3_CONSOLE_UART) > 0)
        {
            uart_ll_read_rxfifo(&RT_S3_CONSOLE_UART, &byte, 1);

            rt_uint16_t next = (rt_uint16_t)((s_ring_head + 1) % RT_S3_CON_RING_SIZE);

            if (next == s_ring_tail)
            {
                /* Full: drop the newest byte and count it. Losing input is bad;
                 * overwriting unread input is worse, because it reorders it. */
                s_isr_overruns++;
                break;
            }

            s_ring[s_ring_head] = byte;
            s_ring_head = next;
            got = RT_TRUE;
        }

        if (got)
        {
            /* Wake the device layer, if one is registered. */
            rt_hw_console_rx_notify();
        }
    }
}

/*
 * Register the RX interrupt. Called from rt_hw_board_init() before the
 * scheduler exists, which is fine: the handler only fills a ring.
 *
 * rt_hw_interrupt_install() is this port's own API (port/esp32s3/interrupt.c),
 * which fills the slot in _xt_interrupt_table that IDF's dispatch_c_isr walks.
 * The line itself is already routed by the ROM console setup, but routing it
 * again is free and makes this file independent of what the ROM did.
 */
rt_err_t rt_hw_console_irq_init(void)
{
    /* Step 1 of four: source -> CPU line. */
    if (rt_hw_intr_route(ETS_UART0_INTR_SOURCE, ETS_UART0_INUM) != 0)
    {
        return -RT_EINVAL;
    }

    /* Step 2: the handler. */
    rt_hw_interrupt_install(ETS_UART0_INUM, s3_uart_isr, RT_NULL, "uart0");

    /* Step 3: unmask the CPU line. Step 4: enable the peripheral side. */
    uart_ll_set_rx_tout(&RT_S3_CONSOLE_UART, RT_S3_CON_RX_TOUT);
    uart_ll_ena_intr_mask(&RT_S3_CONSOLE_UART, RT_S3_CON_RX_MASK);
    rt_hw_interrupt_umask(ETS_UART0_INUM);

    s_uart_ready = RT_TRUE;

    return RT_EOK;
}

/*
 * Console input.
 *
 * Drains the ISR ring first, then falls back to the FIFO. The fallback is what
 * keeps the console usable if the interrupt path is broken - see the header
 * comment. Returning -1 means "no character yet", which is the contract
 * finsh_getchar() expects from this function (it loops with a delay).
 */
signed char rt_hw_console_getchar(void)
{
    if (s_ring_head != s_ring_tail)
    {
        signed char ch = (signed char)s_ring[s_ring_tail];

        s_ring_tail = (rt_uint16_t)((s_ring_tail + 1) % RT_S3_CON_RING_SIZE);

        return ch;
    }

    /*
     * Nothing buffered. Poll the FIFO ONLY while the interrupt is not
     * installed.
     *
     * Polling a live interrupt-driven FIFO is a race, and QEMU says so out loud
     * ("esp_uart: read UART FIFO while it is empty"): between the length check
     * and the read, the ISR can drain the FIFO and the read lands on an empty
     * one. On hardware the same window returns stale data or garbage. The
     * fallback therefore exists for exactly one situation - the interrupt was
     * never installed, which board.c reports at boot - and is compiled out of
     * the normal path by this condition.
     */
    if (!s_uart_ready && uart_ll_get_rxfifo_len(&RT_S3_CONSOLE_UART) > 0)
    {
        uint8_t byte;

        uart_ll_read_rxfifo(&RT_S3_CONSOLE_UART, &byte, 1);
        return (signed char)byte;
    }

    return -1;
}

/*
 * Whether the RX interrupt is really live: handler installed by this driver,
 * line unmasked. board.c asks once at boot so a line that was routed but left
 * masked is reported instead of silently never firing.
 */
rt_bool_t rt_hw_console_irq_ok(void)
{
    return s_uart_ready && !rt_hw_interrupt_is_masked(ETS_UART0_INUM);
}

/* ---- diagnostics --------------------------------------------------------- */

/*
 * Print what the interrupt path has seen. Exposed as `s3rt irq` rather than
 * printed at boot: the interesting value is the one after a human has typed
 * something, not at startup.
 */
void rt_hw_console_irq_stats(rt_uint32_t *isr_count, rt_uint32_t *overruns,
                             rt_bool_t *installed)
{
    if (isr_count)
    {
        *isr_count = s_isr_count;
    }

    if (overruns)
    {
        *overruns = s_isr_overruns;
    }

    if (installed)
    {
        *installed = s_uart_ready && !rt_hw_interrupt_is_masked(ETS_UART0_INUM);
    }
}
