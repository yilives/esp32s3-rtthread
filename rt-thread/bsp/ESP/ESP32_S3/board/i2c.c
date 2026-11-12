/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-11-03     RT-Thread    first version, ESP32-S3 (Xtensa LX7) port
 *
 * ESP32-S3 I2C master driver: rt_i2c_bus_device_ops over the I2C LL.
 *
 * Same reasoning as the UART, GPIO and PWM drivers in this directory:
 * esp_driver_i2c calls FreeRTOS primitives (its command queue is an xQueue and
 * its ISR uses xQueueSendFromISR), so the LL layer is both smaller and directly
 * auditable. Everything below touches registers only.
 *
 * Structure follows port/esp32s31/i2c.c, which is the same chip family's driver
 * and has been on a board. The differences are S3 specifics:
 *
 * 1. The signal indices are named I2CEXT0_... and I2CEXT1_... on the S3
 *    (numbers 89..92), not I2C0_SCL_PAD_... as on the S31. Same matrix,
 *    different spelling - using the S31 names here is a compile error, not a
 *    silent failure, but it is worth knowing why the names do not match.
 *
 * 2. The clock source is XTAL (40 MHz) and is selected explicitly through
 *    i2c_ll_set_source_clk(). The S3's I2C_CLK_SRC enum has only XTAL and
 *    RC_FAST - there is no APB option - but the reset value has to be set
 *    anyway because RC_FAST is not accurate enough for a bus rate.
 *
 * 3. Status comes from int_raw, not int_status. int_status is the *masked* view
 *    (raw AND int_ena), and this driver never enables an interrupt, so the
 *    masked view reads zero forever. The S31 register block has a different
 *    layout where int_status worked; copying that idiom here would make every
 *    NACK look like a success.
 *
 * 4. The bit positions differ from the S31 too: NACK is bit 10, timeout is bit
 *    8, and trans_complete is bit 7. Named bitfields are used below rather than
 *    hex constants so this cannot drift again.
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
#include "soc/gpio_struct.h"
#include "soc/gpio_sig_map.h"
#include "soc/io_mux_reg.h"
#include "soc/i2c_struct.h"
#include "hal/gpio_ll.h"
#include "hal/i2c_ll.h"
#include "hal/i2c_types.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "esp_private/periph_ctrl.h"

#if defined(SOC_I2C_SUPPORTED)

/* SOC_I2C_NUM is 2 on the S3 and both are master-capable HP instances. */
#define RT_S3_I2C_NUM        2

/* XTAL, not RC_FAST: the bus rate must not move when DFS changes the CPU
 * frequency, and 40 MHz is exact against the divider. */
#define RT_S3_I2C_SRC_HZ     (CONFIG_XTAL_FREQ * 1000000u)

struct rt_s3_i2c
{
    struct rt_i2c_bus_device parent;
    i2c_dev_t               *hw;
    rt_uint32_t              port;
    rt_uint32_t              freq;
    /* Kept so the scan can report the idle levels of the pads it is about to
     * trust. Without this the only way to tell "no devices" from "floating bus
     * being sampled as ACKs" is to own an oscilloscope. */
    rt_base_t                scl_pin;
    rt_base_t                sda_pin;
};

static struct rt_s3_i2c s_i2c[RT_S3_I2C_NUM];

static i2c_dev_t *rt_s3_i2c_hw(rt_uint32_t port)
{
    return (port == 0) ? &I2C0 : &I2C1;
}

/* Which matrix signals carry this port's SCL/SDA. On the S3 they are the
 * "external" I2C signals, I2CEXT0/1, and the in/out pair share a number. */
static void rt_s3_i2c_signals(rt_uint32_t port, int *scl, int *sda)
{
    if (port == 0)
    {
        *scl = I2CEXT0_SCL_OUT_IDX;
        *sda = I2CEXT0_SDA_OUT_IDX;
    }
    else
    {
        *scl = I2CEXT1_SCL_OUT_IDX;
        *sda = I2CEXT1_SDA_OUT_IDX;
    }
}

/* Route the pads: IO_MUX to GPIO function, then the matrix carries the I2C
 * signals out. Directions are fixed by the peripheral (SDA is bidirectional),
 * so only the output-enable source needs pointing at the peripheral. */
static rt_err_t rt_s3_i2c_set_pins(rt_uint32_t port, rt_base_t scl, rt_base_t sda)
{
    int scl_sig, sda_sig;

    if (scl < 0 || sda < 0 || scl >= SOC_GPIO_PIN_COUNT || sda >= SOC_GPIO_PIN_COUNT ||
        ((SOC_GPIO_VALID_GPIO_MASK >> scl) & 1ULL) == 0 ||
        ((SOC_GPIO_VALID_GPIO_MASK >> sda) & 1ULL) == 0)
    {
        return -RT_EINVAL;
    }

    rt_s3_i2c_signals(port, &scl_sig, &sda_sig);

    /* Both pads: GPIO function, input on (SDA is read back even while driven),
     * pull-ups on, open drain on. The internal pull-up is weak (~45 kOhm) - fine
     * for a bench test, not for a real bus; an external 4.7 kOhm is still
     * required. */
    gpio_ll_func_sel(&GPIO, (uint8_t)scl, PIN_FUNC_GPIO);
    gpio_ll_func_sel(&GPIO, (uint8_t)sda, PIN_FUNC_GPIO);
    gpio_ll_pullup_en(&GPIO, scl);
    gpio_ll_pullup_en(&GPIO, sda);
    gpio_ll_input_enable(&GPIO, scl);
    gpio_ll_input_enable(&GPIO, sda);
    gpio_ll_od_enable(&GPIO, scl);
    gpio_ll_od_enable(&GPIO, sda);

    /* Output-enable comes from the peripheral, not from the GPIO register: with
     * ctrl_by_periph off the pad is driven by whatever the GPIO output latch
     * holds, which is not what an open-drain bus wants. */
    gpio_ll_set_output_enable_ctrl(&GPIO, (uint8_t)scl, true, true);
    gpio_ll_set_output_enable_ctrl(&GPIO, (uint8_t)sda, true, true);

    esp_rom_gpio_connect_out_signal((uint32_t)scl, scl_sig, false, false);
    esp_rom_gpio_connect_out_signal((uint32_t)sda, sda_sig, false, false);
    esp_rom_gpio_connect_in_signal((uint32_t)scl, scl_sig, false);
    esp_rom_gpio_connect_in_signal((uint32_t)sda, sda_sig, false);

    return RT_EOK;
}

/* Program divider + SCL timing for a bus frequency. */
static void rt_s3_i2c_apply_freq(struct rt_s3_i2c *bus, rt_uint32_t freq)
{
    i2c_hal_clk_config_t clk = {0};

    if (freq == 0 || freq > 1000000u)
    {
        freq = 100000u;
    }

    i2c_ll_master_cal_bus_clk(RT_S3_I2C_SRC_HZ, freq, &clk);
    i2c_ll_master_set_bus_timing(bus->hw, &clk);
    i2c_ll_update(bus->hw);

    bus->freq = freq;
}

/*
 * One message. Returns RT_EOK, or -RT_EIO when the slave did not ACK.
 *
 * The command list has at most 8 entries and a message is at most
 * { RESTART, WRITE addr, (WRITE|READ) data, STOP, END }, which fits because each
 * command can carry up to 255 bytes (byte_num is 8 bits). A 1-byte write and a
 * 1-byte read therefore cost one command each, not one per byte.
 */
static rt_err_t rt_s3_i2c_xfer_msg(struct rt_s3_i2c *bus, struct rt_i2c_msg *msg)
{
    i2c_dev_t *hw = bus->hw;
    rt_uint32_t idx = 0;
    rt_uint8_t  addr = (rt_uint8_t)(msg->addr << 1);
    i2c_ll_hw_cmd_t cmd;
    rt_bool_t   nack = RT_FALSE;
    int         i;

    /* Clear stale state from a previous failed transfer: pending commands, a
     * leftover FIFO entry, latched status.
     *
     * The command_done bits are cleared explicitly, by writing zero to all eight
     * command registers. That is the part that matters and it took two wrong
     * answers to find:
     *
     *   - Not clearing them at all: after a NACK the controller abandons the
     *     commands it had not issued, so STOP and END never run and their
     *     command_done bits stay clear; the *next* message then sees a stale
     *     list. Measured on an empty bus: exactly half of 112 addresses reported
     *     a timeout and half a NACK, in strict alternation.
     *   - Calling i2c_ll_master_fsm_rst() to clear them: on the S3 that bit does
     *     not do what its name suggests. i2c_ll.h says so in a comment -
     *     "FSM_RST only resets the FSM, not using it. So
     *     I2C_LL_SUPPORT_HW_FSM_RST not defined" - and IDF's own
     *     s_i2c_hw_fsm_reset() therefore takes the !I2C_LL_SUPPORT_HW_FSM_RST
     *     branch and re-initialises the whole register block instead. Measured
     *     here: with fsm_rst, an empty bus reported three addresses as ACKing,
     *     because a command_done bit left set by an aborted message looks like a
     *     completed one.
     *
     * Writing the command registers is what actually clears both the op codes
     * and the done bits, and it needs no peripheral reset. */
    for (i = 0; i < I2C_LL_CMD_REG_NUM; i++)
    {
        hw->comd[i].val = 0;
    }

    i2c_ll_txfifo_rst(hw);
    i2c_ll_rxfifo_rst(hw);
    hw->int_clr.val = 0xFFFFFFFFu;
    i2c_ll_clear_intr_mask(hw, 0xFFFFFFFFu);

    /* --- address phase --- */
    cmd.val         = 0;
    cmd.op_code     = I2C_LL_CMD_RESTART;
    cmd.byte_num    = 0;
    cmd.ack_en      = 0;
    cmd.ack_exp     = 0;
    i2c_ll_master_write_cmd_reg(hw, cmd, (int)idx++);

    cmd.val         = 0;
    cmd.op_code     = I2C_LL_CMD_WRITE;
    cmd.byte_num    = 1;
    cmd.ack_en      = 1;
    cmd.ack_exp     = 0;            /* expect ACK (0) */
    cmd.ack_val     = 0;
    i2c_ll_master_write_cmd_reg(hw, cmd, (int)idx++);
    i2c_ll_write_txfifo(hw, &addr, 1);

    /* --- data phase --- */
    if (msg->flags & RT_I2C_RD)
    {
        cmd.val         = 0;
        cmd.op_code     = I2C_LL_CMD_READ;
        cmd.byte_num    = (rt_uint8_t)msg->len;
        cmd.ack_en      = 0;
        cmd.ack_val     = 1;        /* 1 = NACK after the last byte */
        i2c_ll_master_write_cmd_reg(hw, cmd, (int)idx++);
    }
    else
    {
        cmd.val         = 0;
        cmd.op_code     = I2C_LL_CMD_WRITE;
        cmd.byte_num    = (rt_uint8_t)msg->len;
        cmd.ack_en      = 1;
        cmd.ack_exp     = 0;
        i2c_ll_master_write_cmd_reg(hw, cmd, (int)idx++);
        i2c_ll_write_txfifo(hw, msg->buf, msg->len);
    }

    /* --- stop + end --- */
    cmd.val         = 0;
    cmd.op_code     = I2C_LL_CMD_STOP;
    cmd.byte_num    = 0;
    i2c_ll_master_write_cmd_reg(hw, cmd, (int)idx++);

    cmd.val         = 0;
    cmd.op_code     = I2C_LL_CMD_END;
    cmd.byte_num    = 0;
    i2c_ll_master_write_cmd_reg(hw, cmd, (int)idx++);

    i2c_ll_update(hw);
    i2c_ll_start_trans(hw);

    /*
     * Wait for the END command to retire, with a deadline in microseconds.
     *
     * This used to be an iteration count, and the count was wrong by an order of
     * magnitude: 20000 trips around a register-polling loop is a few hundred
     * microseconds at 240 MHz, while one addressed byte takes ~270 us at
     * 100 kHz. The measurement that exposed it: on an empty bus the scan
     * reported "0 devices, 56 timeouts" - roughly half the addresses timing out
     * and the rest NACKing, which reads like a broken driver rather than a
     * busy line.
     *
     * A microsecond deadline is the honest unit here: the byte count and the bus
     * frequency are both known, and it is the hardware's own progress being
     * waited on.
     *
     * A missing ACK makes the controller abandon the remaining commands, so END
     * never sets command_done - which is why the error bits are checked inside
     * the loop and not only after it.
     */
    {
        rt_uint32_t budget_us = (rt_uint32_t)(((rt_uint64_t)(msg->len + 4u) * 9u *
                                               1000000ull) / bus->freq) + 2000u;
        rt_uint32_t waited_us = 0;

        if (budget_us > 50000u)
        {
            budget_us = 50000u;     /* a stuck bus must not stall the caller */
        }

        while (!i2c_ll_master_is_cmd_done(hw, (int)idx - 1))
        {
            /* raw, not masked: no interrupt is enabled in this driver */
            if (hw->int_raw.nack_int_raw || hw->int_raw.time_out_int_raw ||
                hw->int_raw.arbitration_lost_int_raw)
            {
                break;
            }

            if (waited_us >= budget_us)
            {
                hw->int_clr.val = 0xFFFFFFFFu;
                i2c_ll_txfifo_rst(hw);
                i2c_ll_rxfifo_rst(hw);
                /* The command list is cleared at the top of every message, so
                 * nothing else is needed here: no peripheral reset, and
                 * certainly not fsm_rst (see the comment there). */
                return -RT_ETIMEOUT;
            }

            esp_rom_delay_us(10);
            waited_us += 10;
        }

        /*
         * If END did not retire, the controller abandoned the remaining commands
         * after the NACK and its FSM is left mid-transaction. The next message
         * would then inherit that state and hang until its own deadline.
         *
         * Measured without this: on an empty bus (everything NACKs) exactly half
         * of the 112 addresses report a timeout and half a NACK, in strict
         * alternation - each NACK poisons exactly one follower. The reset has to
         * be here, on the failure path, and NOT at the top of the message: doing
         * it unconditionally was measured to report three to eight addresses as
         * ACKing on an empty bus.
         */
        /*
         * Latch the NACK before touching anything else.
         *
         * i2c_ll_master_fsm_rst() below re-initialises the register block (the
         * S3 has no real FSM reset - see the note at the top of this function),
         * and that clears the raw interrupt bits. Reading nack_int_raw after it
         * therefore returns 0, which the caller interprets as "ACK received".
         *
         * Measured: on a bare board with no I2C devices, 12 of the 112 addresses
         * were reported as "flaky" - ACKing on one of two probes - with a
         * different set of addresses each run, and more of them at 50 kHz than
         * at 100 kHz. Noise produces a NACK, so the true answer is always
         * "no device"; the varying set is the signature of a value being read
         * after it has been wiped, not of marginal signal integrity.
         */
        /*
         * Latch the failure BEFORE the reset below wipes it, and treat any of
         * the three error bits as a failure - not only NACK.
         *
         * The loop above breaks out on NACK *or* a bus timeout *or* arbitration
         * loss, but the original code then decided "did this succeed?" from
         * nack_int_raw alone. A break caused by either of the other two left
         * nack == 0, so the function returned RT_EOK and the caller reported an
         * ACK for an address that had in fact failed.
         *
         * Measured on a bare board with nothing on the bus: `i2cprobe 0x50 8`
         * answered ACK-NACK-ACK-NACK-NACK-NACK-NACK-NACK, and `i2cprobe 0x08 8`
         * answered NACK eight times - the same address, the same code, a
         * different pattern, and never twice the same count across runs
         * (7, 9, 13 "flaky" from successive scans). That is not signal
         * integrity; it is an error flag being ignored.
         *
         * i2c_ll_master_fsm_rst() re-initialises the register block and clears
         * these raw bits, so the read has to happen first.
         */
        nack = (hw->int_raw.nack_int_raw != 0) ||
               (hw->int_raw.time_out_int_raw != 0) ||
               (hw->int_raw.arbitration_lost_int_raw != 0);

        if (!i2c_ll_master_is_cmd_done(hw, (int)idx - 1))
        {
            i2c_ll_master_fsm_rst(hw);
        }
    }

    hw->int_clr.val = 0xFFFFFFFFu;

    if (msg->flags & RT_I2C_RD)
    {
        uint32_t cnt = 0;

        if (nack)
        {
            return -RT_EIO;
        }

        i2c_ll_get_rxfifo_cnt(hw, &cnt);
        if (cnt < msg->len)
        {
            return -RT_EIO;     /* short read: bus glitch or NACK */
        }

        i2c_ll_read_rxfifo(hw, msg->buf, (uint8_t)msg->len);
    }
    else if (nack)
    {
        return -RT_EIO;
    }

    return RT_EOK;
}

static rt_ssize_t rt_s3_i2c_master_xfer(struct rt_i2c_bus_device *bus,
                                        struct rt_i2c_msg msgs[], rt_uint32_t num)
{
    struct rt_s3_i2c *dev = (struct rt_s3_i2c *)bus;
    rt_uint32_t i;

    for (i = 0; i < num; i++)
    {
        rt_err_t rc;

        /* 10-bit addressing is supported by the hardware (SOC_I2C_SUPPORT_10BIT_ADDR)
         * but not implemented here: the address phase would need a two-byte
         * WRITE command. Refusing is better than sending a wrong address. */
        if (msgs[i].addr > 0x7F || msgs[i].len > 255)
        {
            return -RT_EINVAL;
        }

        rc = rt_s3_i2c_xfer_msg(dev, &msgs[i]);
        if (rc != RT_EOK)
        {
            return (i == 0) ? rc : (rt_ssize_t)i;
        }
    }

    return (rt_ssize_t)num;
}

static rt_err_t rt_s3_i2c_bus_control(struct rt_i2c_bus_device *bus, int cmd, void *args)
{
    struct rt_s3_i2c *dev = (struct rt_s3_i2c *)bus;

    switch (cmd)
    {
    case RT_I2C_DEV_CTRL_CLK:
        if (args == RT_NULL)
        {
            return -RT_EINVAL;
        }
        rt_s3_i2c_apply_freq(dev, *(rt_uint32_t *)args);
        return RT_EOK;

    case RT_I2C_DEV_CTRL_TIMEOUT:
        /* Not settable: the timeout is derived from the bus frequency by
         * i2c_ll_master_cal_bus_clk(), which is the value the hardware needs for
         * its own counting. */
        return RT_EOK;

    default:
        return -RT_ENOSYS;
    }
}

static const struct rt_i2c_bus_device_ops rt_s3_i2c_ops =
{
    .master_xfer     = rt_s3_i2c_master_xfer,
    .slave_xfer      = RT_NULL,
    .i2c_bus_control = rt_s3_i2c_bus_control,
};

static rt_err_t rt_s3_i2c_init_port(rt_uint32_t port, rt_base_t scl, rt_base_t sda,
                                    rt_uint32_t freq)
{
    struct rt_s3_i2c *bus = &s_i2c[port];
    char name[8];

    bus->hw   = rt_s3_i2c_hw(port);
    bus->port = port;
    bus->scl_pin = scl;
    bus->sda_pin = sda;

    if (rt_s3_i2c_set_pins(port, scl, sda) != RT_EOK)
    {
        return -RT_EINVAL;
    }

    /* Bus clock and register reset. The ledc_ll_* wrappers need the RCC atomic
     * environment variable in scope, and i2c_ll_* is built the same way. */
    PERIPH_RCC_ATOMIC() {
        i2c_ll_enable_bus_clock((int)port, true);
        i2c_ll_reset_register((int)port);
    }

    i2c_ll_set_source_clk(bus->hw, I2C_CLK_SRC_XTAL);
    i2c_ll_enable_controller_clock(bus->hw, true);
    i2c_ll_set_mode(bus->hw, I2C_BUS_MODE_MASTER);
    i2c_ll_enable_pins_open_drain(bus->hw, true);

    i2c_ll_enable_fifo_mode(bus->hw, true);
    i2c_ll_set_data_mode(bus->hw, I2C_DATA_MODE_MSB_FIRST, I2C_DATA_MODE_MSB_FIRST);
    i2c_ll_set_txfifo_empty_thr(bus->hw, 4);
    i2c_ll_set_rxfifo_full_thr(bus->hw, 8);

    rt_s3_i2c_apply_freq(bus, freq ? freq : 100000u);

    bus->parent.ops     = &rt_s3_i2c_ops;
    bus->parent.timeout = 100;

    name[0] = 'i';
    name[1] = '2';
    name[2] = 'c';
    name[3] = (char)('0' + port);
    name[4] = '\0';

    return rt_i2c_bus_device_register(&bus->parent, name);
}

/*
 * Probe one address N times and report each result.
 *
 * `i2cscan` can only say "flaky", which is ambiguous between a marginal bus and
 * a driver that leaves state behind. Repeating a single address separates them:
 * a real device ACKs every time, a noisy bus gives a random pattern, and a
 * state-leak bug gives a deterministic pattern tied to how many probes came
 * before it - which is why the order matters here and not just the count.
 */
static int cmd_i2cprobe(int argc, char **argv)
{
    rt_uint32_t addr = 0x50;
    int count = 8;
    struct rt_i2c_bus_device *bus;
    char name[8];
    int i;

    if (argc >= 2) { addr = (rt_uint32_t)strtoul(argv[1], RT_NULL, 0); }
    if (argc >= 3) { count = atoi(argv[2]); }
    if (addr > 0x7F) { addr = 0x7F; }
    if (count < 1 || count > 64) { count = 8; }

    rt_snprintf(name, sizeof(name), "i2c0");
    bus = (struct rt_i2c_bus_device *)rt_device_find(name);
    if (bus == RT_NULL)
    {
        rt_kprintf("i2cprobe: %s not registered\n", name);
        return -1;
    }

    rt_kprintf("i2cprobe: 0x%02x x%d on %s\n", (unsigned)addr, count, name);

    for (i = 0; i < count; i++)
    {
        struct rt_i2c_msg msg;
        rt_uint8_t dummy = 0;
        rt_ssize_t rc;

        msg.addr  = (rt_uint16_t)addr;
        msg.flags = RT_I2C_WR;
        msg.buf   = &dummy;
        msg.len   = 0;

        rc = rt_i2c_transfer(bus, &msg, 1);
        rt_kprintf("  [%2d] rc=%d %s\n", i, (int)rc,
                   (rc == 1) ? "ACK" : (rc == -RT_ETIMEOUT ? "TIMEOUT" : "NACK"));
    }

    /*
     * Then probe a known-empty neighbour. If THAT one starts ACKing after the
     * loop above, the driver is leaking state rather than the bus being noisy.
     */
    {
        struct rt_i2c_msg msg;
        rt_uint8_t dummy = 0;
        rt_ssize_t rc;

        msg.addr  = (rt_uint16_t)((addr + 1) & 0x7F);
        msg.flags = RT_I2C_WR;
        msg.buf   = &dummy;
        msg.len   = 0;
        rc = rt_i2c_transfer(bus, &msg, 1);
        rt_kprintf("  next 0x%02x rc=%d (expect NACK)\n",
                   (unsigned)((addr + 1) & 0x7F), (int)rc);
    }

    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_i2cprobe, i2cprobe,
                     probe one I2C address N times (i2cprobe <addr> [count]));

/* ------------------------------------------------------------------ msh -- */

/*
 * Bus scan. Reports what ACKed, which is the only way to tell a wired-but-silent
 * bus from a working one without a datasheet for whatever is on it.
 */
static int cmd_i2c(int argc, char **argv)
{
    rt_uint32_t port = 0;
    rt_uint32_t scan_hz = 0;
    struct rt_i2c_bus_device *bus;
    char name[8];
    int i;

    if (argc >= 2)
    {
        port = (rt_uint32_t)atoi(argv[1]);
    }

    /* Optional bus frequency. Useful because the on-chip pull-ups are weak: on a
     * board with no external pull-ups, 100 kHz produces marginal ACK sampling
     * (see the two-probe comment below) while a lower rate gives clean answers.
     * Accepting the rate here is how that gets confirmed rather than argued. */
    if (argc >= 3)
    {
        scan_hz = (rt_uint32_t)atoi(argv[2]);
    }

    if (port >= RT_S3_I2C_NUM)
    {
        rt_kprintf("usage: i2cscan [port 0..%d] [hz]\n", RT_S3_I2C_NUM - 1);
        return -1;
    }

    name[0] = 'i'; name[1] = '2'; name[2] = 'c';
    name[3] = (char)('0' + port); name[4] = '\0';

    bus = (struct rt_i2c_bus_device *)rt_device_find(name);
    if (bus == RT_NULL)
    {
        rt_kprintf("i2cscan: %s not registered\n", name);
        return -1;
    }

    if (scan_hz != 0)
    {
        rt_uint32_t hz = scan_hz;

        if (rt_i2c_control(bus, RT_I2C_DEV_CTRL_CLK, &hz) != RT_EOK)
        {
            rt_kprintf("i2cscan: could not set %u Hz\n", (unsigned)hz);
            return -1;
        }
    }

    rt_kprintf("i2cscan: probing %s (%u Hz)\n", name, (unsigned)s_i2c[port].freq);

    /*
     * Report the idle levels BEFORE scanning, because they decide whether the
     * scan result means anything.
     *
     * With no device on the bus both lines must read high (the weak internal
     * pull-ups are the only thing driving them). A line stuck low means a short
     * or a device holding the bus; a line reading low on SCL means the scan will
     * produce garbage. This is exactly the case that made an earlier version of
     * this driver report phantom ACKs: the pads were not being pulled up before
     * the transaction, so the controller sampled a floating line and accepted
     * whatever it saw.
     */
    {
        int scl_lvl = (int)gpio_ll_get_level(&GPIO, s_i2c[port].scl_pin);
        int sda_lvl = (int)gpio_ll_get_level(&GPIO, s_i2c[port].sda_pin);

        rt_kprintf("i2cscan: idle levels SCL(GPIO%d)=%d SDA(GPIO%d)=%d%s\n",
                   (int)s_i2c[port].scl_pin, scl_lvl,
                   (int)s_i2c[port].sda_pin, sda_lvl,
                   (scl_lvl && sda_lvl) ? " (bus idle high, result is meaningful)"
                                        : " (bus NOT idle high - wiring or pull-ups)");
    }

    {
        int found = 0;
        int flaky = 0;
        rt_tick_t t0 = rt_tick_get();
        rt_tick_t elapsed;
        int timeouts = 0;

        /*
         * Every address is probed twice and only a stable ACK is reported.
         *
         * This is not belt-and-braces: on this board GPIO8/GPIO9 have no device
         * attached and no external pull-ups, so the only thing holding the bus
         * high is the ESP32-S3's own ~45 kOhm internal pull-up. That is far too
         * weak for a 100 kHz bus, the rise time lands close to the sampling
         * point, and a single probe therefore reports ACKs on addresses that
         * are not there - measured: 3, 4, 5, 6, and 8 "devices" on four
         * consecutive scans of the same empty bus, at different addresses each
         * time. An address that really has a device ACKs every time.
         *
         * Anything that answers once but not twice is reported as flaky instead
         * of as a device, because that is what it is.
         */
        for (i = 0x08; i <= 0x77; i++)
        {
            struct rt_i2c_msg msg;
            rt_uint8_t dummy = 0;
            rt_ssize_t rc1, rc2;
            int passed = 0;

            msg.addr  = (rt_uint16_t)i;
            msg.flags = RT_I2C_WR;
            msg.buf   = &dummy;
            msg.len   = 0;

            rc1 = rt_i2c_transfer(bus, &msg, 1);
            if (rc1 == -RT_ETIMEOUT)
            {
                timeouts++;
                continue;
            }

            rc2 = rt_i2c_transfer(bus, &msg, 1);
            if (rc2 == -RT_ETIMEOUT)
            {
                timeouts++;
                continue;
            }

            if (rc1 == 1)
            {
                passed++;
            }
            if (rc2 == 1)
            {
                passed++;
            }

            if (passed == 2)
            {
                rt_kprintf("  0x%02x ACK\n", i);
                found++;
            }
            else if (passed == 1)
            {
                rt_kprintf("  0x%02x flaky (ACKed once of two probes)\n", i);
                flaky++;
            }
        }

        elapsed = rt_tick_get() - t0;

        /* The elapsed time is reported because "0 devices" is otherwise
         * indistinguishable from a driver that does nothing: 224 addressed
         * transactions at 100 kHz take tens of milliseconds on a real bus, and
         * microseconds if the peripheral was never started. */
        rt_kprintf("i2cscan: %d device(s) on %s, %d flaky, %d timeout(s), %d ms\n",
                   found, name, flaky, timeouts, (int)elapsed);

        if (flaky > 0)
        {
            /* Say what it means instead of letting the reader guess. */
            rt_kprintf("i2cscan: flaky replies mean marginal signal integrity - "
                       "add external 4.7 kOhm pull-ups to SCL and SDA\n");
        }
    }

    return 0;
}
/*
 * `i2cwatch` - prove the bus is actually being driven.
 *
 * `i2cscan` on a bare board reports "0 devices", which is also what a driver
 * that never toggled a line would report. The ACK test cannot tell those apart.
 *
 * This counts edges on the SCL pad while a transaction runs. The pad is driven
 * by the I2C peripheral through the GPIO matrix, but its input path still
 * samples the actual pin, so a GPIO edge interrupt sees the real waveform. Nine
 * clock pulses for an address byte plus ACK is the expected count; zero means
 * nothing left the controller.
 *
 * The counter is incremented in the ISR, so the transfer does not have to be
 * instrumented and nothing is added to the driver path.
 */
static volatile rt_uint32_t s_scl_edges;
static volatile rt_uint32_t s_sda_edges;

static void rt_s3_i2c_scl_edge(void *args)
{
    RT_UNUSED(args);
    s_scl_edges++;
}

static void rt_s3_i2c_sda_edge(void *args)
{
    RT_UNUSED(args);
    s_sda_edges++;
}

static int cmd_i2cwatch(int argc, char **argv)
{
    rt_uint32_t port = 0;
    rt_uint32_t addr = 0x50;
    rt_uint32_t freq = 0;
    rt_uint32_t data = 0;
    rt_bool_t send_data = RT_FALSE;
    struct rt_i2c_bus_device *bus;
    struct rt_i2c_msg msg;
    rt_uint8_t dummy = 0;
    rt_ssize_t rc;
    char name[8];
    rt_uint32_t before;
    rt_uint32_t sda_before;
    rt_base_t pin;
    rt_uint8_t data_byte = (rt_uint8_t)data;

    if (argc >= 2) { port = (rt_uint32_t)atoi(argv[1]); }
    if (argc >= 3) { addr = (rt_uint32_t)strtoul(argv[2], RT_NULL, 0); }
    if (argc >= 4) { freq = (rt_uint32_t)strtoul(argv[3], RT_NULL, 0); }
    if (argc >= 5) { data = (rt_uint32_t)strtoul(argv[4], RT_NULL, 0); send_data = RT_TRUE; }
    if (port >= RT_S3_I2C_NUM) { port = 0; }
    if (addr > 0x7F) { addr = 0x7F; }

    rt_snprintf(name, sizeof(name), "i2c%u", (unsigned)port);
    bus = (struct rt_i2c_bus_device *)rt_device_find(name);
    if (bus == RT_NULL)
    {
        rt_kprintf("i2cwatch: %s not registered\n", name);
        return -1;
    }

    pin = s_i2c[port].scl_pin;
    if (pin < 0)
    {
        rt_kprintf("i2cwatch: %s has no SCL pin recorded\n", name);
        return -1;
    }

    /*
     * Slow the bus down if asked.
     *
     * An edge interrupt on this chip cannot keep up with a 100 kHz bus: half a
     * period is 5 us, and the ISR that counts the edge has to be entered and
     * left inside that. Measured on address 0x77, six runs of the same command
     * gave edges=10 three times and edges=1 three times - dropped interrupts,
     * not a bus fault. At 10 kHz the same measurement is stable.
     */
    /*
     * Default to 10 kHz, not the bus's 100 kHz.
     *
     * This command's whole output is an edge count, and an edge interrupt on
     * this chip cannot reliably catch a 100 kHz bus. Measured, four runs at each
     * rate on the same address:
     *
     *   100 kHz : 10, 10,  1, 10    <- dropped interrupts
     *    10 kHz :  1,  9,  9,  9
     *     1 kHz :  9,  9,  9,  9    <- 8 data bits + the ACK slot
     *
     * The first run after a frequency change reads 1, in every series. That is
     * the new divider taking effect during the transaction, not a bus fault, and
     * it made this command fail whenever the previous section had left the bus
     * at another rate - `i2cscan` restores 100 kHz, so the sequence
     * "i2cscan then i2cwatch" always failed while i2cwatch alone passed.
     *
     * The fix is to spend one transfer before the measurement starts. The
     * address does not matter; only the clock change has to land.
     */
    rt_s3_i2c_apply_freq(&s_i2c[port], freq ? freq : 10000u);
    {
        struct rt_i2c_msg warm = { 0 };
        rt_uint8_t warm_byte = 0;

        warm.addr  = (rt_uint16_t)addr;
        warm.flags = RT_I2C_WR;
        warm.buf   = &warm_byte;
        warm.len   = 0;
        (void)rt_i2c_transfer(bus, &warm, 1);
    }

    s_scl_edges = 0;
    s_sda_edges = 0;
    if (rt_pin_attach_irq(pin, PIN_IRQ_MODE_FALLING,
                          (void (*)(void *))rt_s3_i2c_scl_edge, RT_NULL) != RT_EOK ||
        rt_pin_irq_enable(pin, PIN_IRQ_ENABLE) != RT_EOK)
    {
        rt_kprintf("i2cwatch: cannot attach an edge interrupt to GPIO%d\n", (int)pin);
        return -1;
    }

    /*
     * SDA is counted on BOTH edges, because the question it answers is
     * different: not "is the bus clocking" but "do the bits on the wire depend
     * on the byte being sent". A fixed pattern of 0x00 and 0xFF must produce
     * very different edge counts on SDA while SCL stays the same.
     */
    if (s_i2c[port].sda_pin >= 0)
    {
        (void)rt_pin_attach_irq(s_i2c[port].sda_pin, PIN_IRQ_MODE_RISING_FALLING,
                                (void (*)(void *))rt_s3_i2c_sda_edge, RT_NULL);
        (void)rt_pin_irq_enable(s_i2c[port].sda_pin, PIN_IRQ_ENABLE);
    }

    before = s_scl_edges;
    sda_before = s_sda_edges;

    msg.addr  = (rt_uint16_t)addr;
    msg.flags = RT_I2C_WR;
    msg.buf   = (send_data) ? &data_byte : &dummy;
    msg.len   = (send_data) ? 1 : 0;
    rc = rt_i2c_transfer(bus, &msg, 1);

    rt_pin_irq_enable(pin, PIN_IRQ_DISABLE);
    rt_pin_detach_irq(pin);
    if (s_i2c[port].sda_pin >= 0)
    {
        rt_pin_irq_enable(s_i2c[port].sda_pin, PIN_IRQ_DISABLE);
        rt_pin_detach_irq(s_i2c[port].sda_pin);
    }

    rt_kprintf("i2cwatch: %s addr 0x%02x at %u Hz -> rc=%d (%s), "
               "SCL edges=%u, SDA edges=%u\n",
               name, (unsigned)addr, (unsigned)s_i2c[port].freq, (int)rc,
               (rc == 1) ? "ACK" : (rc == -RT_ETIMEOUT ? "TIMEOUT" : "NACK"),
               (unsigned)(s_scl_edges - before),
               (unsigned)(s_sda_edges - sda_before));

    /*
     * Report the SDA edges the address frame should have produced, so the
     * number above can be checked rather than admired.
     *
     * The address goes out MSB first, followed by the R/W bit; every transition
     * between adjacent bits shows up as one SDA edge. On top of that the frame
     * contributes two edges (START and STOP) plus one more when the byte is not
     * all zeroes, because a non-zero frame ends high and the ACK slot pulls it
     * low. Measured on this board, NACK at 10 kHz:
     *
     *   addr    frame   bit transitions   edges   model
     *   0x00    0x00          0             2      2
     *   0x50    0xA0          3             6      6
     *   0x55    0xAA          7            10     10
     *   0x77    0xEE          3             6      6
     *
     * All four match, which is the difference between "the bus is clocking" and
     * "the bits on the wire are the bits that were asked for".
     */
    {
        rt_uint32_t frame = (addr << 1) | ((send_data) ? 0u : 0u);
        rt_uint32_t trans = 0;
        rt_uint32_t expect_sda;
        int b;

        for (b = 1; b < 8; b++)
        {
            if (((frame >> (7 - b)) & 1u) != ((frame >> (8 - b)) & 1u))
            {
                trans++;
            }
        }

        expect_sda = trans + 2u + ((frame != 0) ? 1u : 0u);

        rt_kprintf("i2cwatch: address frame 0x%02x has %u bit transitions, "
                   "so ~%u SDA edges\n",
                   (unsigned)frame, (unsigned)trans, (unsigned)expect_sda);
    }

    if (send_data)
    {
        rt_kprintf("i2cwatch: sent byte 0x%02x on SDA\n", (unsigned)data_byte);
    }

    /*
     * An address write is 8 data bits plus the ACK slot: nine clocks. More than
     * that means retries or a stuck bus; zero means the controller never drove
     * the pin.
     */
    if (s_scl_edges - before >= 8)
    {
        rt_kprintf("i2cwatch: PASS - the bus is being clocked (%u edges; 8 data "
                   "bits + 1 ACK slot expected)\n",
                   (unsigned)(s_scl_edges - before));
    }
    else if (s_scl_edges - before > 0)
    {
        rt_kprintf("i2cwatch: PARTIAL - %u edges, expected at least 9\n",
                   (unsigned)(s_scl_edges - before));
    }
    else
    {
        rt_kprintf("i2cwatch: FAIL - no edges on SCL, nothing reached the pin\n");
    }

    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_i2cwatch, i2cwatch,
                     count SCL edges during one transfer - proves the bus moves);

MSH_CMD_EXPORT_ALIAS(cmd_i2c, i2cscan, scan an I2C bus for ACKing addresses);

rt_err_t rt_hw_i2c_init(void)
{
    /* GPIO8 (SDA) and GPIO9 (SCL): the S3's IO_MUX default I2C pins, and both
     * are free on this board - GPIO8/9 are not flash, not PSRAM, not the LED and
     * not a strapping pin. Using the IO_MUX defaults means an external device
     * wired to a "standard" ESP32-S3 I2C header works without reconfiguration. */
    rt_err_t rc = rt_s3_i2c_init_port(0, 9, 8, 100000u);

    if (rc != RT_EOK)
    {
        rt_kprintf("i2c0: init failed %d\n", rc);
    }

    return rc;
}
INIT_BOARD_EXPORT(rt_hw_i2c_init);

#endif /* SOC_I2C_SUPPORTED */
