/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-11-03     RT-Thread    first version, ESP32-S3 (Xtensa LX7) port
 *
 * ESP32-S3 GPIO driver: rt_pin_ops over the GPIO LL.
 *
 * Same reasoning as the UART driver in this directory: esp_driver_gpio is
 * FreeRTOS free, but it drags in the interrupt allocator and the caps layers,
 * and the LL is both smaller and directly auditable. Everything below touches
 * IO_MUX, the GPIO registers and the interrupt matrix, nothing else.
 *
 * Three S3 specific points worth remembering:
 *
 * 1. The pad has to be routed to the GPIO peripheral before input/output enable
 *    has any effect: IO_MUX.gpio[n].mcu_sel must be PIN_FUNC_GPIO. That is step
 *    one of IDF's gpio_input_enable()/gpio_output_enable(), and skipping it
 *    leaves the pad driven by whichever peripheral was mapped last.
 *
 * 2. GPIO39..42 are the SPI flash/PSRAM pads on this module and are NOT usable
 *    as GPIO. The board also wires the strapping pins (GPIO0 = BOOT button,
 *    GPIO45/46 = flash voltage) and the LED on GPIO48 - see the schematic.
 *    SOC_GPIO_VALID_GPIO_MASK already excludes the flash pads, so the validity
 *    check below is what keeps an accidental rt_pin_mode(40, ...) from bricking
 *    the console.
 *
 * 3. The pin interrupt line is ASKED FOR, not hardcoded. ETS_GPIO_INUM is a
 *    number the IDF interrupt allocator may already have given to the OS tick;
 *    the S31 port lost three rounds to exactly that collision (see
 *    port/esp32s31/gpio.c for the full account). esp_intr_alloc() knows which
 *    lines are taken, so the line comes from it.
 */

#include <rthw.h>
#include <rtthread.h>
#include <rtdevice.h>

#include "sdkconfig.h"
#include "soc/soc.h"
#include "soc/soc_caps.h"
#include "soc/gpio_struct.h"
#include "soc/gpio_sig_map.h"
#include "soc/io_mux_reg.h"
#include "soc/interrupts.h"
#include "hal/gpio_ll.h"
#include "esp_attr.h"
#include "esp_intr_alloc.h"
#include "esp_err.h"

#include "port_intr.h"

/* S3 exposes 49 pads; SOC_GPIO_VALID_GPIO_MASK has the flash ones cleared. */
#define RT_S3_PIN_NUM           SOC_GPIO_PIN_COUNT

struct rt_s3_pin_irq
{
    void      (*hdr)(void *args);
    void       *args;
    rt_uint8_t  mode;       /* PIN_IRQ_MODE_* */
    rt_bool_t   attached;
};

static struct rt_s3_pin_irq s_pin_irq[RT_S3_PIN_NUM];
static rt_bool_t             s_gpio_isr_ready;

/* Line the allocator gave us, and its handle. -1 until the first attach. */
static int              s_gpio_line = -1;
static intr_handle_t s_gpio_handle;

static rt_bool_t rt_s3_pin_valid(rt_base_t pin)
{
    if (pin < 0 || pin >= RT_S3_PIN_NUM)
    {
        return RT_FALSE;
    }

    return ((SOC_GPIO_VALID_GPIO_MASK >> pin) & 1ULL) ? RT_TRUE : RT_FALSE;
}

/* Clear the latched interrupt bit of a single pin. The status registers are
 * split at pin 32 and the clear registers are write-1-to-clear. */
static void rt_s3_pin_clear_irq(rt_base_t pin)
{
    if (pin < 32)
    {
        gpio_ll_clear_intr_status(&GPIO, 1U << pin);
    }
    else
    {
        gpio_ll_clear_intr_status_high(&GPIO, 1U << (pin - 32));
    }
}

static void rt_s3_pin_mode(struct rt_device *device, rt_base_t pin, rt_uint8_t mode)
{
    RT_UNUSED(device);

    if (!rt_s3_pin_valid(pin))
    {
        return;
    }

    /* Detach the pad from whatever peripheral owned it, then start from a clean
     * state: no pulls, no open drain, neither direction enabled. */
    gpio_ll_func_sel(&GPIO, (uint8_t)pin, PIN_FUNC_GPIO);
    gpio_ll_pullup_dis(&GPIO, pin);
    gpio_ll_pulldown_dis(&GPIO, pin);
    gpio_ll_od_disable(&GPIO, pin);
    gpio_ll_input_disable(&GPIO, pin);
    gpio_ll_output_disable(&GPIO, pin);

    switch (mode)
    {
    case PIN_MODE_INPUT:
        gpio_ll_input_enable(&GPIO, pin);
        break;

    case PIN_MODE_INPUT_PULLUP:
        gpio_ll_pullup_en(&GPIO, pin);
        gpio_ll_input_enable(&GPIO, pin);
        break;

    case PIN_MODE_INPUT_PULLDOWN:
        gpio_ll_pulldown_en(&GPIO, pin);
        gpio_ll_input_enable(&GPIO, pin);
        break;

    case PIN_MODE_OUTPUT_OD:
        gpio_ll_od_enable(&GPIO, pin);
        /* Open drain still needs the receiver on to read the bus back. */
        gpio_ll_input_enable(&GPIO, pin);
        /* Idle high BEFORE the driver is turned on. The output latch resets to
         * 0, so enabling open drain first and writing a level later holds the
         * bus low from the moment rt_pin_mode() returns - which for an I2C or
         * 1-Wire bus looks exactly like a missing pull-up. */
        gpio_ll_set_level(&GPIO, pin, 1);
        /* fall through */

    case PIN_MODE_OUTPUT:
        /* Point the output selector at "no peripheral" (SIG_GPIO_OUT_IDX) and
         * hand the output enable to the GPIO register block. */
        gpio_ll_set_output_signal_matrix_source(&GPIO, pin, SIG_GPIO_OUT_IDX, false);
        gpio_ll_set_output_enable_ctrl(&GPIO, (uint8_t)pin, false, false);
        /* Keep the receiver on: rt_pin_read() is documented as "get the level of
         * the pin", with no restriction to input pins, and on this SoC the input
         * buffer is off by default. Without this, `pin write 48 high` followed
         * by `pin read 48` reports low. */
        gpio_ll_input_enable(&GPIO, pin);
        gpio_ll_output_enable(&GPIO, pin);
        break;

    default:
        break;
    }
}

static void rt_s3_pin_write(struct rt_device *device, rt_base_t pin, rt_uint8_t value)
{
    RT_UNUSED(device);

    if (!rt_s3_pin_valid(pin))
    {
        return;
    }

    gpio_ll_set_level(&GPIO, pin, value ? 1U : 0U);
}

static rt_ssize_t rt_s3_pin_read(struct rt_device *device, rt_base_t pin)
{
    RT_UNUSED(device);

    if (!rt_s3_pin_valid(pin))
    {
        return -RT_EINVAL;
    }

    return gpio_ll_get_level(&GPIO, pin);
}

/* PIN_IRQ_MODE_* is dense and ordered differently from gpio_int_type_t, so map
 * it instead of relying on numeric values. */
static const gpio_int_type_t rt_s3_intr_type[] =
{
    [PIN_IRQ_MODE_RISING]         = GPIO_INTR_POSEDGE,
    [PIN_IRQ_MODE_FALLING]        = GPIO_INTR_NEGEDGE,
    [PIN_IRQ_MODE_RISING_FALLING] = GPIO_INTR_ANYEDGE,
    [PIN_IRQ_MODE_HIGH_LEVEL]     = GPIO_INTR_HIGH_LEVEL,
    [PIN_IRQ_MODE_LOW_LEVEL]      = GPIO_INTR_LOW_LEVEL,
};

/*
 * One handler for the whole port: the hardware latches per pin and this fans the
 * status words out to the registered callbacks.
 *
 * Signature is `void (*)(void *)`, i.e. IDF's intr_handler_t, and not
 * RT-Thread's `void (*)(int, void *)`: the line is allocated through
 * esp_intr_alloc(), which installs into IDF's own table, so this is the shape it
 * will call. Going through rtt_trampoline() as well would mean two table entries
 * for one line and a vector argument nobody uses.
 *
 * IRAM_ATTR because this runs from an interrupt, and interrupts are enabled
 * while the flash cache is off during SPI-flash writes and MSPI/PSRAM timing
 * work. The pin callbacks it dispatches are application code and may live in
 * flash - that is the caller's choice - but the driver's own dispatch path must
 * not.
 */
static void IRAM_ATTR rt_s3_gpio_isr(void *arg)
{
    uint32_t lo, hi;

    RT_UNUSED(arg);

    gpio_ll_get_intr_status(&GPIO, 0, &lo);
    gpio_ll_get_intr_status_high(&GPIO, 0, &hi);

    if (lo == 0 && hi == 0)
    {
        return;
    }

    /* Clear before dispatching: a level-triggered pin would otherwise re-enter
     * immediately, and an edge arriving during the callbacks is picked up by the
     * next iteration anyway. */
    gpio_ll_clear_intr_status(&GPIO, lo);
    gpio_ll_clear_intr_status_high(&GPIO, hi);

    while (lo)
    {
        int n = __builtin_ctz(lo);

        lo &= lo - 1;
        if (s_pin_irq[n].attached && s_pin_irq[n].hdr)
        {
            s_pin_irq[n].hdr(s_pin_irq[n].args);
        }
    }

    while (hi)
    {
        int n = 32 + __builtin_ctz(hi);

        hi &= hi - 1;
        if (n < RT_S3_PIN_NUM && s_pin_irq[n].attached && s_pin_irq[n].hdr)
        {
            s_pin_irq[n].hdr(s_pin_irq[n].args);
        }
    }
}

/*
 * Route, install and unmask once. The line stays open afterwards; the per-pin
 * int_ena bits decide which pads can actually raise it.
 *
 * The CPU line comes from esp_intr_alloc(), not from ETS_GPIO_INUM: the tick is
 * allocated from the same pool, and hardcoding a number means re-routing the
 * tick's source off its own line and overwriting its handler - which presents
 * as the whole scheduler stopping. See port/esp32s31/gpio.c for the round where
 * that was diagnosed.
 */
static rt_err_t rt_s3_gpio_irq_setup(void)
{
    if (s_gpio_isr_ready)
    {
        return RT_EOK;
    }

    /* ESP_INTR_FLAG_IRAM: the handler is in IRAM and can fire with the cache
     * off. LEVEL1: the weakest level above the C threshold, which is all a pin
     * interrupt needs. */
    esp_err_t err = esp_intr_alloc(ETS_GPIO_INTR_SOURCE,
                                   ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL1,
                                   rt_s3_gpio_isr, RT_NULL,
                                   &s_gpio_handle);
    if (err != ESP_OK)
    {
        rt_kprintf("gpio: esp_intr_alloc failed (%d)\n", (int)err);
        return -RT_ERROR;
    }

    /* The number, for diagnostics only. */
    s_gpio_line = esp_intr_get_intno(s_gpio_handle);

    s_gpio_isr_ready = RT_TRUE;

    return RT_EOK;
}

static rt_err_t rt_s3_pin_attach_irq(struct rt_device *device, rt_base_t pin,
                                     rt_uint8_t mode,
                                     void (*hdr)(void *args), void *args)
{
    rt_base_t level;

    RT_UNUSED(device);

    if (!rt_s3_pin_valid(pin))
    {
        return -RT_EINVAL;
    }

    if (rt_s3_gpio_irq_setup() != RT_EOK)
    {
        return -RT_ERROR;
    }

    level = rt_hw_interrupt_disable();

    s_pin_irq[pin].hdr      = hdr;
    s_pin_irq[pin].args     = args;
    s_pin_irq[pin].mode     = (rt_uint8_t)mode;
    s_pin_irq[pin].attached = RT_TRUE;

    rt_hw_interrupt_enable(level);

    return RT_EOK;
}

static rt_err_t rt_s3_pin_detach_irq(struct rt_device *device, rt_base_t pin)
{
    rt_base_t level;

    RT_UNUSED(device);

    if (!rt_s3_pin_valid(pin))
    {
        return -RT_EINVAL;
    }

    level = rt_hw_interrupt_disable();

    s_pin_irq[pin].attached = RT_FALSE;
    s_pin_irq[pin].hdr      = RT_NULL;
    s_pin_irq[pin].args     = RT_NULL;

    rt_hw_interrupt_enable(level);

    return RT_EOK;
}

static rt_err_t rt_s3_pin_irq_enable(struct rt_device *device, rt_base_t pin,
                                     rt_uint8_t enabled)
{
    if (!rt_s3_pin_valid(pin))
    {
        return -RT_EINVAL;
    }

    if (enabled)
    {
        if (rt_s3_pin_attach_irq(device, pin, s_pin_irq[pin].mode,
                                 s_pin_irq[pin].hdr, s_pin_irq[pin].args) != RT_EOK)
        {
            /* attach also brought the line up; continue only if that worked */
            if (!s_gpio_isr_ready)
            {
                return -RT_ERROR;
            }
        }

        rt_s3_pin_clear_irq(pin);
        gpio_ll_set_intr_type(&GPIO, pin, rt_s3_intr_type[s_pin_irq[pin].mode]);
        gpio_ll_intr_enable_on_core(&GPIO, 0, pin);
    }
    else
    {
        gpio_ll_intr_disable(&GPIO, pin);
        rt_s3_pin_clear_irq(pin);
    }

    return RT_EOK;
}

static const struct rt_pin_ops rt_s3_pin_ops =
{
    .pin_mode       = rt_s3_pin_mode,
    .pin_write      = rt_s3_pin_write,
    .pin_read       = rt_s3_pin_read,
    .pin_attach_irq = rt_s3_pin_attach_irq,
    .pin_detach_irq = rt_s3_pin_detach_irq,
    .pin_irq_enable = rt_s3_pin_irq_enable,
    .pin_get        = RT_NULL,
};

int rt_hw_pin_init(void)
{
    int ret = rt_device_pin_register("pin", &rt_s3_pin_ops, RT_NULL);

    if (ret == RT_EOK && s_gpio_line >= 0)
    {
        rt_kprintf("gpio: pin device ready on CPU line %d\n", s_gpio_line);
    }

    return ret;
}
INIT_BOARD_EXPORT(rt_hw_pin_init);
