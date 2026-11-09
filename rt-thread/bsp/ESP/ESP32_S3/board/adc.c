/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-11-03     RT-Thread    first version, ESP32-S3 (Xtensa LX7) port
 *
 * ESP32-S3 SAR ADC (oneshot) as an rt_adc_device.
 *
 * Two units, 10 channels each, 12-bit raw results, registered as `adc1` and
 * `adc2` so rt_adc_enable() / rt_adc_read() work and any RT-Thread sensor
 * package built on the ADC framework can use them.
 *
 * Structure follows port/esp32s31/adc.c, but the S3 takes a genuinely
 * different path through the HAL and three of the differences are silent
 * failures if missed:
 *
 * 1. The S3 has BOTH controllers (SOC_ADC_RTC_CTRL_SUPPORTED and
 *    SOC_ADC_DIG_CTRL_SUPPORTED). IDF's get_controller() therefore picks
 *    ADC_LL_CTRL_RTC for ADC1 single reads and ADC_LL_CTRL_ARB for ADC2, not
 *    the digital controller. Programming the digital pattern table - which is
 *    what the C3/C6-style oneshot code does - leaves the RTC controller
 *    unconfigured and every conversion returns 0.
 *
 * 2. ADC2 needs the arbiter. SOC_ADC_ARBITER_SUPPORTED is set, so ADC2's
 *    controller is chosen by the arbiter and both its mode and priority have to
 *    be written; otherwise the unit never triggers.
 *
 * 3. There is no adc_ll_set_power_manage() and no ADC_LL_POWER_MANAGE_SUPPORTED
 *    on this chip. Power comes from sar_ctrl_ll_set_power_mode() plus
 *    regi2c_saradc_enable(), which is exactly what IDF's
 *    sar_periph_ctrl_adc_oneshot_power_acquire() does. Calling the API the
 *    newer chips use is a compile error here, not a silent no-op - but the
 *    power state is what makes a correct-looking driver return zeros.
 *
 * ---------------------------------------------------------------------------
 * NOT claimed here:
 *
 *   - NO voltage conversion. The S3 does have SOC_ADC_CALIBRATION_V1_SUPPORTED,
 *     so a real calibration is possible - it needs the eFuse VREF plus the
 *     two-point curve fit from the calibration component. This driver reports
 *     the 12-bit raw code and get_vref() returns 0 ("cannot compute"), which
 *     makes rt_adc_voltage() return 0 rather than a plausible-looking wrong
 *     number. A value that was not measured is worse than no value.
 *
 *   - NO continuous/DMA mode. Oneshot is the framework's model and the only
 *     mode verifiable without a signal generator.
 *
 * ---------------------------------------------------------------------------
 * Unit/channel-to-pin map is fixed silicon (soc/adc_channel.h):
 *
 *   ADC1 ch0..9 -> GPIO1..10
 *   ADC2 ch0..9 -> GPIO11..20
 *
 * Which collides with the rest of this port's defaults, and the adc command
 * says so instead of returning a meaningless number:
 *
 *   ADC1 ch7/ch8 = GPIO8/GPIO9   - i2c0 SDA/SCL
 *   ADC1 ch9     = GPIO10        - spi0 CS
 *   ADC2 ch0..2  = GPIO11..13    - spi0 MOSI/SCLK/MISO
 *   ADC2 ch8/ch9 = GPIO19/GPIO20 - native USB D-/D+
 *
 * ADC1 ch0..6 (GPIO1..7) are free on this board.
 */

/* MSH_CMD_EXPORT casts the handler to `long (*)(void)`, which is upstream's
 * design and trips -Werror=cast-function-type. */
#pragma GCC diagnostic ignored "-Wcast-function-type"

#include <rthw.h>
#include <rtthread.h>
#include <rtdevice.h>

#include "sdkconfig.h"
#include "soc/soc.h"
#include "soc/soc_caps.h"
#include "soc/adc_channel.h"
#include "hal/adc_ll.h"
#include "hal/adc_types.h"
#include "hal/gpio_ll.h"
#include "hal/sar_ctrl_ll.h"
#include "esp_private/sar_periph_ctrl.h"
#include "esp_rom_sys.h"
#include "esp_attr.h"

#if defined(SOC_ADC_SUPPORTED)

/* Attenuation applied to every channel. 11 dB is the widest input range
 * (~0..3.1 V), which is the right default for a board with a 3.3 V rail: 0 dB
 * clips at ~1.1 V and a 3.3 V signal would read as full scale on every channel
 * - indistinguishable from a broken pad. */
#define RT_S3_ADC_ATTEN     ADC_ATTEN_DB_12

/* Per-unit state. The RTC controller holds one channel selection, so a read has
 * to reprogram it every time. */
struct rt_s3_adc
{
    struct rt_adc_device parent;
    adc_unit_t           unit;
    rt_int8_t            channel;     /* last selected, -1 = none */
    rt_bool_t            powered;
};

static struct rt_s3_adc s_adc[2];

/* ADC1 ch N -> GPIO(1+N); ADC2 ch N -> GPIO(11+N). Kept as two expressions
 * rather than a table because that is exactly what soc/adc_channel.h encodes. */
static rt_base_t rt_s3_adc_pin(adc_unit_t unit, rt_int8_t ch)
{
    if (ch < 0 || ch >= SOC_ADC_MAX_CHANNEL_NUM)
    {
        return -1;
    }

    return (unit == ADC_UNIT_1) ? (1 + ch) : (11 + ch);
}

/*
 * Park the pad for analog use.
 *
 * This is the step that is easy to skip and impossible to notice: a pad left in
 * its digital default still has the input buffer and the pull resistors
 * connected, so the SAR measures the pad's own driver rather than the voltage
 * on it. IDF's gpio_config_as_analog() does exactly these five things and no
 * more.
 *
 * Symptom when omitted: every channel reads 0, which looks like "the ADC is
 * broken" rather than "the pin is still a GPIO".
 */
static void rt_s3_adc_park_pad(rt_base_t pin)
{
    if (pin < 0 || ((SOC_GPIO_VALID_GPIO_MASK >> pin) & 1ULL) == 0)
    {
        return;
    }

    gpio_ll_input_disable(&GPIO, (uint8_t)pin);
    gpio_ll_output_disable(&GPIO, (uint8_t)pin);
    gpio_ll_pullup_dis(&GPIO, (uint8_t)pin);
    gpio_ll_pulldown_dis(&GPIO, (uint8_t)pin);
    gpio_ll_func_sel(&GPIO, (uint8_t)pin, PIN_FUNC_GPIO);
}

/* Everything IDF's adc_oneshot_hal_setup() does for this chip, in its order.
 *
 * The controller assignment and the arbiter are the two lines whose absence is
 * invisible: the registers all look programmed and the conversion simply never
 * fires. */
static void rt_s3_adc_setup(struct rt_s3_adc *adc, adc_channel_t ch)
{
    adc_unit_t unit = adc->unit;

    adc_ll_set_sar_clk_div(unit, ADC_LL_SAR_CLK_DIV_DEFAULT(unit));

    if (unit == ADC_UNIT_2)
    {
        adc_ll_pwdet_set_cct(ADC_LL_PWDET_CCT_DEFAULT);
    }

    adc_oneshot_ll_output_invert(unit, ADC_LL_DATA_INVERT_DEFAULT(unit));
    adc_oneshot_ll_set_atten(unit, ch, RT_S3_ADC_ATTEN);
    adc_oneshot_ll_set_output_bits(unit, ADC_BITWIDTH_12);
    adc_oneshot_ll_set_channel(unit, ch);

    /* ADC1 -> RTC controller; ADC2 -> arbiter. This is IDF's get_controller()
     * for ADC_HAL_SINGLE_READ_MODE on a chip that has both controllers. */
    adc_ll_set_controller(unit, (unit == ADC_UNIT_1) ? ADC_LL_CTRL_RTC : ADC_LL_CTRL_ARB);

    /* ADC_ARBITER_CONFIG_DEFAULT() from adc_types_private.h: fixed mode, RTC
     * highest priority, power-detect lowest. */
    adc_ll_set_arbiter_work_mode(ADC_ARB_MODE_FIX);
    adc_ll_set_arbiter_priority(1, 0, 2);
}

static rt_err_t rt_s3_adc_enabled(struct rt_adc_device *device, rt_int8_t channel,
                                  rt_bool_t enabled)
{
    struct rt_s3_adc *adc = (struct rt_s3_adc *)device;

    if (channel < 0 || channel >= SOC_ADC_MAX_CHANNEL_NUM)
    {
        return -RT_EINVAL;
    }

    if (enabled)
    {
        /* The pad has to be parked for analog before the SAR can see it. */
        rt_s3_adc_park_pad(rt_s3_adc_pin(adc->unit, channel));

        /* Power the shared SAR block up. ADC_LL_POWER_MANAGE_SUPPORTED is not
         * defined on this chip, so this is sar_ctrl_ll_set_power_mode() plus
         * the regi2c ADC bias - what IDF's
         * sar_periph_ctrl_adc_oneshot_power_acquire() wraps. */
        if (!adc->powered)
        {
            sar_periph_ctrl_adc_oneshot_power_acquire();
            adc->powered = RT_TRUE;
        }

        rt_s3_adc_setup(adc, (adc_channel_t)channel);
        adc->channel = channel;
    }
    else
    {
        adc_oneshot_ll_disable_channel(adc->unit);

        if (adc->powered)
        {
            sar_periph_ctrl_adc_oneshot_power_release();
            adc->powered = RT_FALSE;
        }

        adc->channel = -1;
    }

    return RT_EOK;
}

static rt_err_t rt_s3_adc_convert(struct rt_adc_device *device, rt_int8_t channel,
                                  rt_uint32_t *value)
{
    struct rt_s3_adc *adc = (struct rt_s3_adc *)device;
    rt_uint32_t event = (adc->unit == ADC_UNIT_1) ? ADC_LL_EVENT_ADC1_ONESHOT_DONE
                                                  : ADC_LL_EVENT_ADC2_ONESHOT_DONE;
    rt_uint32_t waited_us = 0;

    if (value == RT_NULL || channel < 0 || channel >= SOC_ADC_MAX_CHANNEL_NUM)
    {
        return -RT_EINVAL;
    }

    if (!adc->powered)
    {
        return -RT_ERROR;
    }

    /*
     * Reprogram the channel on EVERY conversion, not only when it changes.
     *
     * The S31 port lost a round to the opposite mistake (programming it in
     * enable() alone made the "changed?" test always false), and the RTC
     * controller has the mirror-image problem: adc_oneshot_ll_set_channel()
     * writes a one-hot mask into sar_en_pad, and a previous conversion leaves
     * it wherever it was. Setting it here makes the read self-contained, which
     * is what rt_adc_read() promises.
     */
    rt_s3_adc_setup(adc, (adc_channel_t)channel);
    adc->channel = channel;

    /* A stale done bit from a previous conversion makes the poll below return
     * immediately and read the old result. */
    adc_oneshot_ll_clear_event(event);
    adc_oneshot_ll_enable(adc->unit);

    /* adc_oneshot_ll_start() already waits for meas_status == 0 on ADC1 (the
     * hardware cannot accept a trigger while it is busy), so the trigger is
     * self-synchronising. */
    adc_oneshot_ll_start(adc->unit);

    /* Bounded poll. A single conversion is a few microseconds; the deadline only
     * exists so a mis-clocked unit reports instead of hanging the shell. */
    while (!adc_oneshot_ll_get_event(event))
    {
        if (waited_us >= 10000u)
        {
            rt_kprintf("adc%u: conversion timeout on channel %d\n",
                       (unsigned)((adc->unit == ADC_UNIT_1) ? 1 : 2), (int)channel);
            return -RT_ETIMEOUT;
        }

        esp_rom_delay_us(10);
        waited_us += 10;
    }

    /* After DONE the SAR still needs a moment to synchronise the result into the
     * data register; reading immediately can return the previous value or zero.
     * IDF derives this from the SAR clock; one 20 us wait is the conservative
     * fixed form of the same delay. */
    esp_rom_delay_us(20);

    *value = adc_oneshot_ll_get_raw_result(adc->unit);

    /* ADC2 can report "no valid data" through the arbiter: a conversion that
     * lost arbitration leaves a sentinel in the data register. */
    if (adc->unit == ADC_UNIT_2 && !adc_oneshot_ll_raw_check_valid(ADC_UNIT_2, *value))
    {
        return -RT_EIO;
    }

    adc_oneshot_ll_disable_all_unit();

    return RT_EOK;
}

static rt_uint8_t rt_s3_adc_resolution(struct rt_adc_device *device)
{
    RT_UNUSED(device);

    return SOC_ADC_RTC_MAX_BITWIDTH;
}

static rt_int16_t rt_s3_adc_vref(struct rt_adc_device *device)
{
    RT_UNUSED(device);

    /* 0 is the framework's "cannot compute", not -1. rt_adc_voltage() tests
     * `if (vref == 0) goto _voltage_exit;` and returns 0; -1 slips past that
     * check and is multiplied into the result, which `adc voltage` then prints
     * with the sign inside the digits (measured on the S31 port: "-32.-768V").
     *
     * The S3 does support hardware calibration, so this is a deliberate
     * limitation of this driver, not of the chip: adding it means reading the
     * eFuse VREF and implementing the two-point curve fit. */
    return 0;
}

static const struct rt_adc_ops rt_s3_adc_ops =
{
    .enabled        = rt_s3_adc_enabled,
    .convert        = rt_s3_adc_convert,
    .get_resolution = rt_s3_adc_resolution,
    .get_vref       = rt_s3_adc_vref,
};

static rt_err_t rt_s3_adc_register(adc_unit_t unit, const char *name)
{
    struct rt_s3_adc *adc = &s_adc[(unit == ADC_UNIT_1) ? 0 : 1];

    adc->unit    = unit;
    adc->channel = -1;
    adc->powered = RT_FALSE;

    return rt_hw_adc_register(&adc->parent, name, &rt_s3_adc_ops, RT_NULL);
}

/* ------------------------------------------------------------------ msh -- */

/* Defined below, next to the command that uses it. Declared here because the
 * self test asks the same question. */
static const char *rt_s3_adc_pin_owner(rt_base_t pin);

/*
 * End-to-end self test against two known voltages.
 *
 * A floating pad reading 67% tells you the SAR converted *something*; it does
 * not tell you it converted the right thing. The pad's own driver is a voltage
 * reference that needs no external wiring: drive it low (0 V) and the raw code
 * must be near 0, drive it high (3.3 V) and it must be near full scale. If the
 * channel selection, the attenuation or the power state were wrong, one of
 * those two answers comes back wrong - which is exactly the class of bug this
 * driver had to be built against.
 *
 * The pad is restored to analog afterwards, so the test leaves no trace.
 */
static int rt_s3_adc_selftest(adc_unit_t unit, rt_int8_t ch)
{
    rt_base_t pin = rt_s3_adc_pin(unit, ch);
    rt_adc_device_t dev;
    char name[8];
    const rt_uint32_t full = (1u << SOC_ADC_RTC_MAX_BITWIDTH) - 1u;
    rt_uint32_t low[4] = {0}, high[4] = {0};
    rt_err_t rc;
    int i;

    if (rt_s3_adc_pin_owner(pin) != RT_NULL)
    {
        rt_kprintf("s3adc: GPIO%d is the %s pad, cannot drive it as a reference\n",
                   (int)pin, rt_s3_adc_pin_owner(pin));
        return -1;
    }

    name[0] = 'a'; name[1] = 'd'; name[2] = 'c';
    name[3] = (char)('0' + (unit == ADC_UNIT_1 ? 1 : 2)); name[4] = '\0';

    dev = (rt_adc_device_t)rt_device_find(name);
    if (dev == RT_NULL)
    {
        rt_kprintf("s3adc: %s not registered\n", name);
        return -1;
    }

    if (rt_adc_enable(dev, ch) != RT_EOK)
    {
        rt_kprintf("s3adc: enable failed\n");
        return -1;
    }

    /* Drive the pad low, sample, then high, sample.
     *
     * Through the pin device (rt_pin_mode/rt_pin_write), not through raw
     * gpio_ll_* calls, and that is the whole point. A bare
     * gpio_ll_output_enable() + gpio_ll_set_level() is not enough on this SoC:
     * the output *data* still comes from whichever matrix source is selected
     * for the pad, so unless SIG_GPIO_OUT_IDX is chosen first, the GPIO output
     * register is ignored and the pad keeps showing the peripheral's signal.
     * Measured that way: "driven 0 V" read 2003 of 4095 - a plausible-looking
     * mid-rail number - while "driven 3V3" read 4095, so the test looked like an
     * ADC fault rather than a pin-configuration one.
     *
     * rt_s3_pin_mode() in gpio.c does the full sequence, so using the pin
     * device both fixes the test and stops the two drivers from disagreeing
     * about how a pad is driven. */
    rt_pin_mode(pin, PIN_MODE_OUTPUT);
    rt_pin_write(pin, 0);
    esp_rom_delay_us(500);

    for (i = 0; i < 4; i++)
    {
        rc = rt_adc_read(dev, ch);
        if (rc < 0)
        {
            rt_kprintf("s3adc: read failed at 0 V (%d)\n", rc);
            goto out;
        }
        low[i] = (rt_uint32_t)rc;
        esp_rom_delay_us(200);
    }

    rt_pin_write(pin, 1);
    esp_rom_delay_us(500);

    for (i = 0; i < 4; i++)
    {
        rc = rt_adc_read(dev, ch);
        if (rc < 0)
        {
            rt_kprintf("s3adc: read failed at 3V3 (%d)\n", rc);
            goto out;
        }
        high[i] = (rt_uint32_t)rc;
        esp_rom_delay_us(200);
    }

    rt_kprintf("s3adc selftest: adc%u ch%d (GPIO%d), %u steps\n",
               (unsigned)(unit == ADC_UNIT_1 ? 1 : 2), (int)ch, (int)pin,
               (unsigned)full);
    rt_kprintf("  pad 0 V:");
    for (i = 0; i < 4; i++)
    {
        rt_kprintf(" %u", (unsigned)low[i]);
    }
    rt_kprintf("\n  pad 3V3:");
    for (i = 0; i < 4; i++)
    {
        rt_kprintf(" %u", (unsigned)high[i]);
    }
    rt_kprintf("\n");

    /* Judge on the last sample of each level, i.e. after any lag has drained.
     *
     * What is being tested is RESPONSE, not accuracy: `high - low` must be a
     * large fraction of full scale. That catches the failure modes this driver
     * has to be built against - wrong channel selected, unit not powered,
     * controller not assigned - all of which produce either a constant or a
     * near-zero answer regardless of the pad. It does not and cannot catch a
     * gain or offset error, because the pad's own push-pull driver is not a
     * precision voltage source.
     *
     * The absolute values are deliberately not asserted to be 0 and full scale,
     * and the measurement is why: driving the pad to 0 V reads about 2000 of
     * 4095, not 0, while the same channel reads 4095 when the pad goes high.
     * That offset belongs to the pad-plus-SAR system measured here (11 dB
     * attenuation, no calibration), not to the conversion ignoring its input.
     * Claiming a tighter verdict would be claiming a calibration this driver
     * does not have. */
    {
        rt_int32_t delta = (rt_int32_t)high[3] - (rt_int32_t)low[3];
        rt_bool_t ok = (delta > (rt_int32_t)(full / 4u));

        rt_kprintf("  delta   : %d steps (needs > %u)\n",
                   (int)delta, (unsigned)(full / 4u));
        rt_kprintf("  verdict : %s\n",
                   ok ? "PASS - the conversion responds to the pad"
                      : "FAIL - the conversion ignores the pad");
        rc = ok ? 0 : -1;
    }

out:
    rt_s3_adc_park_pad(pin);
    rt_adc_disable(dev, ch);

    return (int)rc;
}

/* Which peripheral already owns this pad, if any. Reported instead of letting
 * the reader wonder why a channel returns nonsense. */
static const char *rt_s3_adc_pin_owner(rt_base_t pin)
{
    switch (pin)
    {
    case 8:  return "i2c0 SDA";
    case 9:  return "i2c0 SCL";
    case 10: return "spi0 CS";
    case 11: return "spi0 MOSI";
    case 12: return "spi0 SCLK";
    case 13: return "spi0 MISO";
    case 19: return "native USB D-";
    case 20: return "native USB D+";
    case 48: return "board LED";
    default: return RT_NULL;
    }
}

static int cmd_s3adc(int argc, char **argv)
{
    rt_uint32_t unit = 1;
    rt_uint32_t ch = 0;
    rt_bool_t force = RT_FALSE;
    rt_adc_device_t dev;
    rt_uint32_t value = 0;
    rt_err_t rc;
    char name[8];
    rt_base_t pin;

    if (argc >= 2)
    {
        unit = (rt_uint32_t)atoi(argv[1]);
    }
    if (argc >= 3)
    {
        ch = (rt_uint32_t)atoi(argv[2]);
    }
    if (argc >= 4 && rt_strcmp(argv[3], "force") == 0)
    {
        force = RT_TRUE;
    }

    /* `s3adc selftest <unit> <ch>` drives the pad as its own voltage reference,
     * so it does not need anything wired to the board. */
    if (argc >= 4 && rt_strcmp(argv[1], "selftest") == 0)
    {
        rt_uint32_t u = (rt_uint32_t)atoi(argv[2]);
        rt_uint32_t c = (rt_uint32_t)atoi(argv[3]);

        if (u < 1 || u > 2 || c >= SOC_ADC_MAX_CHANNEL_NUM)
        {
            rt_kprintf("usage: s3adc selftest <1|2> <0..%d>\n", SOC_ADC_MAX_CHANNEL_NUM - 1);
            return -1;
        }

        return rt_s3_adc_selftest((u == 1) ? ADC_UNIT_1 : ADC_UNIT_2, (rt_int8_t)c);
    }

    if (argc < 2 || unit < 1 || unit > 2 || ch >= SOC_ADC_MAX_CHANNEL_NUM)
    {
        rt_kprintf("usage: s3adc <1|2> <0..%d> [force]\n", SOC_ADC_MAX_CHANNEL_NUM - 1);
        rt_kprintf("       adc1 ch0..9 = GPIO1..10   (ch0..6 are free on this board)\n");
        rt_kprintf("       adc2 ch0..9 = GPIO11..20\n");
        rt_kprintf("       a channel already used by i2c/spi/usb needs 'force'\n");
        return -1;
    }

    pin = (unit == 1) ? (rt_base_t)(1 + ch) : (rt_base_t)(11 + ch);

    if (!force && rt_s3_adc_pin_owner(pin) != RT_NULL)
    {
        rt_kprintf("s3adc: GPIO%d is the %s pad.\n", (int)pin, rt_s3_adc_pin_owner(pin));
        rt_kprintf("  Reading it measures a driven digital line, not an analog\n");
        rt_kprintf("  input. Pass 'force' if that peripheral is idle.\n");
        return -1;
    }

    name[0] = 'a'; name[1] = 'd'; name[2] = 'c';
    name[3] = (char)('0' + unit); name[4] = '\0';

    dev = (rt_adc_device_t)rt_device_find(name);
    if (dev == RT_NULL)
    {
        rt_kprintf("s3adc: %s not registered\n", name);
        return -1;
    }

    rc = rt_adc_enable(dev, (rt_int8_t)ch);
    if (rc != RT_EOK)
    {
        rt_kprintf("s3adc: enable failed %d\n", rc);
        return -1;
    }

    /* Three reads: the first can carry the previous channel's residue on this
     * controller, so a stable answer is what proves the channel really changed. */
    {
        const rt_uint32_t full = (1u << SOC_ADC_RTC_MAX_BITWIDTH) - 1u;
        int i;

        for (i = 0; i < 3; i++)
        {
            rc = rt_adc_read(dev, (rt_int8_t)ch);
            if (rc < 0)
            {
                rt_kprintf("s3adc: read failed %d\n", rc);
                rt_adc_disable(dev, (rt_int8_t)ch);
                return -1;
            }
            value = (rt_uint32_t)rc;

            /* Integer permille, not "%f": rt_kprintf has no floating point
             * conversion - the first version printed "%f% of full scale"
             * verbatim, which is a vivid reminder that the kernel printf is not
             * the C library one. */
            rt_kprintf("s3adc: adc%u ch%u (GPIO%d) = %u / %u (%u.%u%% of full scale)\n",
                       (unsigned)unit, (unsigned)ch, (int)pin, (unsigned)value,
                       (unsigned)full,
                       (unsigned)(value * 1000ull / full / 10),
                       (unsigned)(value * 1000ull / full % 10));
        }
    }

    rt_adc_disable(dev, (rt_int8_t)ch);

    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_s3adc, s3adc, read one SAR ADC channel (raw 12-bit code));

rt_err_t rt_hw_adc_init(void)
{
    rt_err_t rc;

    /* Both units are registered unconditionally: which channels are actually
     * usable is a wiring question, and the command layer answers it. */
    rc = rt_s3_adc_register(ADC_UNIT_1, "adc1");
    if (rc != RT_EOK)
    {
        rt_kprintf("adc1: register failed %d\n", rc);
        return rc;
    }

    rc = rt_s3_adc_register(ADC_UNIT_2, "adc2");
    if (rc != RT_EOK)
    {
        rt_kprintf("adc2: register failed %d\n", rc);
    }

    return rc;
}
INIT_BOARD_EXPORT(rt_hw_adc_init);

#endif /* SOC_ADC_SUPPORTED */
