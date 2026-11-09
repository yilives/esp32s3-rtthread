/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-11-03     RT-Thread    first version, ESP32-S3 (Xtensa LX7) port
 *
 * ESP32-S3 LEDC PWM driver: rt_device_pwm over the LEDC LL.
 *
 * Modelled on port/esp32s31/pwm.c, which is the same chip family's driver and
 * has been on a board. The differences are all S3 specifics and are called out
 * where they occur:
 *
 * 1. The S3 has ONLY the low-speed LEDC group (ledc_periph.c: one entry,
 *    LEDC_LS_SIG_OUT0..7). SOC_LEDC_SUPPORT_HS_MODE is not defined, so
 *    LEDC_HIGH_SPEED_MODE does not exist as a usable mode - passing it would
 *    index past the end of the signal table.
 *
 * 2. LEDC's clock can be APB, XTAL or RC_FAST (SOC_LEDC_SUPPORT_APB_CLOCK and
 *    SOC_LEDC_SUPPORT_XTAL_CLOCK are both set). XTAL is chosen so the output
 *    frequency does not move when DFS changes the APB divider - the same reason
 *    the S31 driver uses it.
 *
 * 3. The clock must be SELECTED and its divider configured before a timer can
 *    run; ledc_ll_set_clock_divider() only programs the divider.
 *
 * The frequency model is RT-Thread's: PWM_CMD_SET_PERIOD/PULSE carry
 * nanoseconds. A channel's period is owned by its timer, and there are 4 timers
 * for 8 channels, so channels 0 and 4 share timer 0, and so on. That is a
 * documented property of the LEDC, not a choice made here.
 */

#include <rtthread.h>
#include <rtdevice.h>

#include "sdkconfig.h"
#include "soc/soc.h"
#include "soc/soc_caps.h"
#include "soc/gpio_struct.h"
#include "soc/io_mux_reg.h"
#include "hal/gpio_ll.h"
#include "hal/ledc_ll.h"
#include "hal/ledc_types.h"
#include "hal/ledc_periph.h"
#include "esp_rom_gpio.h"
#include "esp_private/periph_ctrl.h"

#define RT_S3_LEDC_MODE     LEDC_LOW_SPEED_MODE
#define RT_S3_LEDC_GROUP    0
/* XTAL, not APB: 40 MHz and independent of the CPU frequency, so a PWM period
 * does not change when DFS moves APB. */
#define RT_S3_LEDC_SRC_HZ   (CONFIG_XTAL_FREQ * 1000000u)
#define RT_S3_LEDC_RES_MAX  14          /* 16384 steps */
#define RT_S3_LEDC_DIV_MAX  0x3FFFFu    /* 10-bit integer + 8-bit fraction */

struct rt_s3_pwm
{
    struct rt_device_pwm parent;
};

/* The framework sends only the field that changed, so the last period and pulse
 * are kept per channel to let all three setters share one code path and to give
 * PWM_CMD_GET something to answer with. */
struct rt_s3_pwm_state
{
    rt_uint32_t period;             /* ns */
    rt_uint32_t pulse;              /* ns */
    rt_bool_t   enabled;
};

static struct rt_s3_pwm       s_pwm;
static struct rt_s3_pwm_state s_state[SOC_LEDC_CHANNEL_NUM];
static ledc_dev_t            *s_ledc;

/* Integer part in bits 8.., fraction in bits 0..7 - the LEDC divider format.
 * Same expression as esp_driver_ledc's ledc_calculate_divisor().
 *
 * Returns 0 for "cannot represent", and the caller fails loudly. Clamping to the
 * nearest legal value instead (an earlier version of the S31 driver did) turns
 * an impossible request into a silently different frequency: at resolution 14
 * the ceiling is reached below ~2.38 Hz, so asking for 1 Hz would deliver
 * 2.38 Hz while reporting success. */
static rt_uint32_t rt_s3_divisor(rt_uint32_t src_hz, rt_uint32_t freq_hz,
                                 rt_uint32_t res)
{
    rt_uint32_t precision = (rt_uint32_t)1u << res;
    rt_uint64_t div = (((rt_uint64_t)src_hz << 8) +
                       ((rt_uint64_t)freq_hz * precision / 2)) /
                      ((rt_uint64_t)freq_hz * precision);

    /* Below 1.0 the timer would stall; above the 10.8 ceiling it cannot divide
     * far enough. Both mean "this period is not achievable at this
     * resolution". */
    if (div > RT_S3_LEDC_DIV_MAX || div < 0x100u)
    {
        return 0;
    }

    return (rt_uint32_t)div;
}

/* Highest resolution whose divider still reaches at least 1.0. */
static rt_uint32_t rt_s3_resolution(rt_uint32_t src_hz, rt_uint32_t freq_hz)
{
    rt_uint32_t res = 1;

    while (res < RT_S3_LEDC_RES_MAX && (src_hz >> res) >= freq_hz)
    {
        res++;
    }

    return res;
}

/* Bring the LEDC block out of reset and select the clock.
 *
 * PERIPH_RCC_ATOMIC(), not PERIPH_RCC_ACQUIRE_ATOMIC(PERIPH_LEDC_MODULE, ...):
 * the S3's shared_periph_module_t enum (soc/periph_defs.h) has no LEDC entry at
 * all - LEDC is a plain SYSTEM.perip_clk_en0 gate here, not an RCC-managed
 * shared peripheral. What the ledc_ll_* wrappers need is only the
 * __DECLARE_RCC_ATOMIC_ENV variable in scope, which PERIPH_RCC_ATOMIC() is
 * exactly the macro for.
 */
static void rt_s3_ledc_init(void)
{
    PERIPH_RCC_ATOMIC() {
        ledc_ll_enable_bus_clock(RT_S3_LEDC_GROUP, true);
        ledc_ll_reset_register(RT_S3_LEDC_GROUP);
    }

    ledc_ll_set_slow_clk_sel(s_ledc, LEDC_SLOW_CLK_XTAL);
}

rt_err_t rt_hw_pwm_set_pin(rt_uint32_t channel, rt_base_t pin)
{
    /* Checked against the *valid* mask, not the pad count: the S3's in-package
     * flash and PSRAM pads sit inside 0..SOC_GPIO_PIN_COUNT-1 but are cleared
     * from SOC_GPIO_VALID_GPIO_MASK. Driving one of those from the LEDC takes
     * the pad away from the flash controller, and the next instruction fetch
     * faults instead of producing a wrong output level. */
    if (channel >= SOC_LEDC_CHANNEL_NUM || pin < 0 || pin >= SOC_GPIO_PIN_COUNT ||
        ((SOC_GPIO_VALID_GPIO_MASK >> pin) & 1ULL) == 0)
    {
        return -RT_EINVAL;
    }

    /* Pad to the GPIO function first, then let the matrix carry LEDC out to it.
     * Skipping the func_sel is the classic silent failure: the matrix is wired
     * up but the pad is still muxed to something else. */
    gpio_ll_func_sel(&GPIO, pin, PIN_FUNC_GPIO);
    gpio_ll_output_enable(&GPIO, pin);

    esp_rom_gpio_connect_out_signal((uint32_t)pin,
                                    ledc_periph_signal[RT_S3_LEDC_GROUP]
                                        .speed_mode[RT_S3_LEDC_MODE]
                                        .sig_out_idx[channel],
                                    false, false);

    return RT_EOK;
}

/* Write period and pulse into the LEDC registers for one channel. */
static rt_err_t rt_s3_pwm_apply(rt_uint32_t ch, rt_uint32_t period_ns,
                                rt_uint32_t pulse_ns)
{
    ledc_dev_t *hw = s_ledc;
    rt_uint32_t timer = ch % SOC_LEDC_TIMER_NUM;
    rt_uint32_t freq_hz;
    rt_uint32_t res;
    rt_uint32_t precision;
    rt_uint32_t duty;
    rt_uint32_t div;

    if (period_ns == 0 || pulse_ns > period_ns)
    {
        return -RT_EINVAL;
    }

    freq_hz = (rt_uint32_t)(1000000000ull / period_ns);
    if (freq_hz == 0)
    {
        return -RT_EINVAL;          /* below 1 Hz */
    }

    res       = rt_s3_resolution(RT_S3_LEDC_SRC_HZ, freq_hz);
    precision = (rt_uint32_t)1u << res;
    duty      = (rt_uint32_t)(((rt_uint64_t)pulse_ns * precision) / period_ns);

    /* 100% is a legal duty: it means "never toggle", which is what a caller
     * using the channel as a level wants. Clamping to precision - 1 silently
     * makes the rail unreachable. */
    if (duty > precision)
    {
        duty = precision;
    }

    div = rt_s3_divisor(RT_S3_LEDC_SRC_HZ, freq_hz, res);
    if (div == 0)
    {
        rt_kprintf("pwm: %u ns period (about %u Hz) is not reachable at "
                   "resolution %u\n", (unsigned)period_ns, (unsigned)freq_hz,
                   (unsigned)res);
        return -RT_EINVAL;
    }

    /* Every channel bound to this timer shares its period - 4 timers, 8
     * channels, so channels 0 and 4 share timer 0. */
    ledc_ll_set_duty_resolution(hw, RT_S3_LEDC_MODE, timer, res);
    ledc_ll_set_clock_divider(hw, RT_S3_LEDC_MODE, timer, div);
    ledc_ll_timer_resume(hw, RT_S3_LEDC_MODE, timer);
    ledc_ll_ls_timer_update(hw, RT_S3_LEDC_MODE, timer);

    ledc_ll_bind_channel_timer(hw, RT_S3_LEDC_MODE, ch, timer);
    ledc_ll_set_hpoint(hw, RT_S3_LEDC_MODE, ch, 0);
    ledc_ll_set_duty_int_part(hw, RT_S3_LEDC_MODE, ch, duty);
    ledc_ll_ls_channel_update(hw, RT_S3_LEDC_MODE, ch);

    return RT_EOK;
}

static rt_err_t rt_s3_pwm_control(struct rt_device_pwm *dev, int cmd, void *arg)
{
    struct rt_pwm_configuration *cfg = (struct rt_pwm_configuration *)arg;
    struct rt_s3_pwm_state *st;
    ledc_dev_t *hw = s_ledc;
    rt_uint32_t ch;
    rt_err_t rc = RT_EOK;

    RT_UNUSED(dev);

    if (cfg == RT_NULL || cfg->channel >= SOC_LEDC_CHANNEL_NUM)
    {
        return -RT_EINVAL;
    }

    ch = cfg->channel;
    st = &s_state[ch];

    switch (cmd)
    {
    case PWM_CMD_ENABLE:
        /* Enabling a channel the framework never configured would drive a pin
         * from a stalled timer (divider 0) and return RT_EOK, which reads as
         * "enabled but the pin does nothing". */
        if (st->period == 0)
        {
            return -RT_EINVAL;
        }

        ledc_ll_set_idle_level(hw, RT_S3_LEDC_MODE, ch, 0);
        /* set_duty_start takes no boolean on this chip: it only has the
         * "start" direction, and disabling goes through the channel's
         * duty_start bit in conf1 too. */
        ledc_ll_set_duty_start(hw, RT_S3_LEDC_MODE, ch);
        ledc_ll_ls_channel_update(hw, RT_S3_LEDC_MODE, ch);
        st->enabled = RT_TRUE;
        break;

    case PWM_CMD_DISABLE:
        ledc_ll_set_idle_level(hw, RT_S3_LEDC_MODE, ch, 0);
        ledc_ll_ls_channel_update(hw, RT_S3_LEDC_MODE, ch);
        st->enabled = RT_FALSE;
        break;

    case PWM_CMD_SET:
        st->period = cfg->period;
        st->pulse  = cfg->pulse;
        rc = rt_s3_pwm_apply(ch, cfg->period, cfg->pulse);
        break;

    case PWM_CMD_SET_PERIOD:
        st->period = cfg->period;
        rc = rt_s3_pwm_apply(ch, st->period, st->pulse);
        break;

    case PWM_CMD_SET_PULSE:
        st->pulse = cfg->pulse;
        rc = rt_s3_pwm_apply(ch, st->period, st->pulse);
        break;

    case PWM_CMD_GET:
        cfg->period = st->period;
        cfg->pulse  = st->pulse;
        break;

    case PWM_CMD_SET_DEAD_TIME:
        /* The LEDC has no complementary output, so there is nothing to phase
         * against. Reporting that is better than accepting and ignoring it. */
        return -RT_ENOSYS;

    default:
        return -RT_EINVAL;
    }

    return rc;
}

static const struct rt_pwm_ops rt_s3_pwm_ops =
{
    .control = rt_s3_pwm_control,
};

int rt_hw_pwm_init(void)
{
    int ret;

    s_ledc = &LEDC;

    rt_s3_ledc_init();

    ret = rt_device_pwm_register(&s_pwm.parent, "pwm", &rt_s3_pwm_ops, RT_NULL);
    if (ret != RT_EOK)
    {
        return ret;
    }

    /* Channel 0 -> GPIO48 would be the board LED, but the LED is not
     * PWM-capable through this pin in a way that helps here: keep the default
     * mapping and let the application call rt_hw_pwm_set_pin(). */

    return RT_EOK;
}
INIT_BOARD_EXPORT(rt_hw_pwm_init);
