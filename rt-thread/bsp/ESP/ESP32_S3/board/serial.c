/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-11-03     RT-Thread    first version, ESP32-S3 (Xtensa LX7) port
 *
 * ESP32-S3 UART0 as an RT-Thread serial v1 device.
 *
 * Relationship to console.c
 * ------------------------
 * console.c already owns the UART0 receive interrupt and its 128-byte ring, and
 * that path is measured working on hardware. This file does NOT install a second
 * handler: two ISRs on one line would fight over the FIFO and the interrupt
 * status register. Instead the device's getc() drains the ring console.c fills,
 * and putc() writes through the same LL FIFO the ROM writer uses. One interrupt,
 * one ring, one owner - with the device stack layered on top.
 *
 * Why the LL and not esp_driver_uart
 * ----------------------------------
 * esp_driver_uart calls xSemaphoreTake / xQueueSendFromISR / xTaskGetTickCount.
 * With the FreeRTOS scheduler removed those either do not exist or block on a
 * scheduler that is not the one running, so the image would link and then hang.
 * Everything here touches registers only.
 *
 * What is left as inherited configuration
 * ---------------------------------------
 * The ROM console already set the baud rate, frame format and pin mux, and those
 * are inherited rather than reprogrammed: a wrong guess here leaves the board
 * with no console at all, which is the one failure mode that is hard to recover
 * from. configure() therefore accepts the rate it is actually running at and
 * refuses others by name.
 */

#include <rthw.h>
#include <rtthread.h>
#include <rtdevice.h>

#include "sdkconfig.h"
#include "esp_attr.h"
#include "esp_rom_sys.h"
#include "soc/soc.h"
#include "soc/uart_struct.h"
#include "soc/interrupts.h"
#include "hal/uart_ll.h"

#include "port_intr.h"

#ifndef CONFIG_ESP_CONSOLE_UART_BAUDRATE
#define CONFIG_ESP_CONSOLE_UART_BAUDRATE 115200
#endif
#define RT_S3_UART0_BAUD        CONFIG_ESP_CONSOLE_UART_BAUDRATE

#define RT_S3_CONSOLE_UART      UART0

/* RX timeout in bit times: raise an interrupt for a short burst rather than
 * waiting for the FIFO to fill, which is what makes an interactive shell work. */
#define RT_S3_UART_RX_TOUT      10

#define RT_S3_UART_RX_MASK      (UART_RXFIFO_FULL_INT_RAW | UART_RXFIFO_TOUT_INT_RAW)

struct rt_serial_device s3_serial0;

static rt_err_t s3_uart_configure(struct rt_serial_device *serial,
                                  struct serial_configure *cfg)
{
    RT_UNUSED(serial);

    /* Inherit the ROM console configuration; only the RX timeout is ours. */
    uart_ll_set_rx_tout(&RT_S3_CONSOLE_UART, RT_S3_UART_RX_TOUT);

    if (cfg->baud_rate != RT_S3_UART0_BAUD)
    {
        /* Honest failure instead of a silent no-op. Changing the rate means
         * reprogramming the divider against the APB clock and re-muxing the
         * pins, and if either is wrong the board loses its console. The rate
         * the ROM chose is already the right one for this board. */
        rt_kprintf("uart0: %d baud not supported, console stays at %d\n",
                   (int)cfg->baud_rate, RT_S3_UART0_BAUD);
        return -RT_EINVAL;
    }

    return RT_EOK;
}

static rt_err_t s3_uart_control(struct rt_serial_device *serial, int cmd, void *arg)
{
    RT_UNUSED(serial);
    RT_UNUSED(arg);

    switch (cmd)
    {
    case RT_DEVICE_CTRL_CLR_INT:
        uart_ll_disable_intr_mask(&RT_S3_CONSOLE_UART, RT_S3_UART_RX_MASK);
        rt_hw_interrupt_mask(ETS_UART0_INUM);
        break;

    case RT_DEVICE_CTRL_SET_INT:
        uart_ll_set_rx_tout(&RT_S3_CONSOLE_UART, RT_S3_UART_RX_TOUT);
        uart_ll_ena_intr_mask(&RT_S3_CONSOLE_UART, RT_S3_UART_RX_MASK);
        rt_hw_interrupt_umask(ETS_UART0_INUM);
        break;

    default:
        break;
    }

    return RT_EOK;
}

static int s3_uart_putc(struct rt_serial_device *serial, char c)
{
    uint8_t byte = (uint8_t)c;

    RT_UNUSED(serial);

    /* Poll instead of enabling TX interrupts: the shell writes a few bytes at a
     * time and this keeps the driver free of a TX ring and a TX ISR. */
    while (uart_ll_get_txfifo_len(&RT_S3_CONSOLE_UART) == 0)
    {
    }

    uart_ll_write_txfifo(&RT_S3_CONSOLE_UART, &byte, 1);

    return 1;
}

/* Filled by the UART0 ISR in console.c; this driver only consumes it. */
extern signed char rt_hw_console_getchar(void);

static int s3_uart_getc(struct rt_serial_device *serial)
{
    RT_UNUSED(serial);

    /* One byte from the ring console.c's ISR fills, or -1.
     *
     * dev_serial.c calls this from inside rt_hw_serial_isr() (ISR context) to
     * move bytes from the driver into the kernel ring, and again from its poll
     * path in thread context. Both are served by the same non-blocking read, so
     * there is exactly one copy of the data path and no second FIFO reader
     * racing the UART ISR. */
    return rt_hw_console_getchar();
}
/*
 * Hand the wake-up to the serial core.
 *
 * This is the strong override of the weak hook in console.c, and it is what
 * makes INT_RX mode work at all: dev_serial.c's reader blocks on rx_sem and is
 * only woken by rt_hw_serial_isr(), which is documented as callable from an ISR
 * and does not block.
 *
 * The serial_rx test is a real guard, not defensive noise. The UART interrupt is
 * unmasked by rt_hw_console_irq_init() during board init, which happens well
 * before finsh opens the device, so a character arriving in that window reaches
 * this hook with dev_serial.c's rx_fifo not yet allocated. Calling
 * rt_hw_serial_isr() there trips its own RT_ASSERT(rx_fifo != RT_NULL):
 *
 *   (rx_fifo != RT_NULL) assertion failed at function:rt_hw_serial_isr,
 *   line number:1548
 *
 * That was invisible until RT_ASSERT stopped being a no-op in this
 * configuration - the assert was simply compiled away, and the NULL never got
 * dereferenced only because an empty ring makes getc() return -1 before the
 * buffer is touched. Turning assertions on is what surfaced it.
 *
 * Skipping the notify in that window is correct rather than a workaround: before
 * the device exists there is no reader to wake, and console.c's ring keeps the
 * byte for the poll path that the shell uses until then.
 *
 * IRAM: called from the UART ISR, which can run with the flash cache off.
 */
void IRAM_ATTR rt_hw_console_rx_notify(void)
{
    if (s3_serial0.serial_rx == RT_NULL)
    {
        return;
    }

    rt_hw_serial_isr(&s3_serial0, RT_SERIAL_EVENT_RX_IND);
}

static const struct rt_uart_ops s3_uart_ops =
{
    .configure    = s3_uart_configure,
    .control      = s3_uart_control,
    .putc         = s3_uart_putc,
    .getc         = s3_uart_getc,
    .dma_transmit = RT_NULL,
};

rt_err_t rt_hw_uart0_init(void)
{
    struct serial_configure config = RT_SERIAL_CONFIG_DEFAULT;

    /* Report the rate the console is really running at, so a mismatch with
     * RT_SERIAL_CONFIG_DEFAULT's 115200 shows up at open() rather than as
     * garbled output. */
    config.baud_rate = RT_S3_UART0_BAUD;

    s3_serial0.ops    = &s3_uart_ops;
    s3_serial0.config = config;

    if (rt_hw_serial_register(&s3_serial0, "uart0",
                              RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_INT_RX,
                              RT_NULL) != RT_EOK)
    {
        return -RT_ERROR;
    }

    /* The interrupt itself is installed by console.c, which rt_hw_board_init()
     * calls before this. Routing is repeated here so this driver does not
     * depend on that ordering, and so a board that ever changes the console
     * source still gets its matrix entry. */
    if (rt_hw_intr_route(ETS_UART0_INTR_SOURCE, ETS_UART0_INUM) != 0)
    {
        return -RT_ERROR;
    }

    /* Make it the kernel console: from here rt_kprintf goes through this device
     * and finsh reads from it (shell.c takes rt_console_get_device() as its
     * input device). rt_hw_console_output() remains as the ROM-based fallback
     * for the window before this runs and for panic paths. */
    rt_console_set_device("uart0");

    return RT_EOK;
}
INIT_BOARD_EXPORT(rt_hw_uart0_init);
