/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-11-03     RT-Thread    first version, ESP32-S3 (Xtensa LX7) port
 *
 * ESP32-S3 SPI master over the GPSPI LL.
 *
 * One bus is registered per general-purpose controller: `spi0` -> GPSPI2 and
 * `spi1` -> GPSPI3. SPI0/SPI1 are the cache/flash controllers and are never
 * touched here. They are plain rt_spi_bus buses, so rt_spi_transfer(),
 * rt_spi_send_then_recv() and any RT-Thread SPI device package (SD over SPI,
 * displays, radio chips) work unchanged.
 *
 * Structure follows port/esp32s31/spi.c. The S3 differences:
 *
 * 1. Signal names. The S3 calls its general-purpose SPI pins after the FSPI
 *    flash lines they share the matrix with: FSPICLK/FSPID/FSPIQ. There are no
 *    SPI2_CK_PAD_* symbols as on the S31. Same matrix, different spelling.
 *
 * 2. `spi_ll_enable_clock()` is needed in addition to `spi_ll_enable_bus_clock()`.
 *    The bus clock gates the register file; the master state machine runs on a
 *    separate gate. Without it usr=1 starts a transaction that never raises
 *    trans_done - which is exactly a timeout with no other symptom.
 *
 * 3. The clock divider is computed against the APB frequency, so that value is
 *    queried rather than assumed: esp_clk_apb_freq() follows DFS, and a divider
 *    derived from a stale constant silently gives the wrong bit rate.
 *
 * Deliberately not implemented, same as the S31 driver:
 *
 *   - DMA. The GPSPI DMA is a separate allocator plus descriptor rings; polled
 *     transfers are enough for small peripherals and far easier to audit.
 *     SOC_SPI_MAXIMUM_BUFFER_SIZE (64) caps one hardware transaction anyway.
 *   - Multi-line (Dual/Quad/OCT) modes. spi_ll_master_set_line_mode() exists and
 *     is programmed for 1-line here; anything else needs the flash-style
 *     command/addr/dummy phases, which is a different API.
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
#include "soc/gpio_sig_map.h"
#include "soc/io_mux_reg.h"
#include "soc/spi_struct.h"
#include "hal/spi_ll.h"
#include "hal/spi_types.h"
#include "hal/gpio_ll.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "esp_cpu.h"
#include "esp_private/esp_clk.h"
#include "esp_private/periph_ctrl.h"
#include "esp_attr.h"

#if defined(SOC_GPSPI_SUPPORTED)

#define RT_S3_SPI_NUM        2

struct rt_s3_spi_pins
{
    rt_base_t sclk;
    rt_base_t mosi;
    rt_base_t miso;
    rt_base_t cs;
};

struct rt_s3_spi
{
    struct rt_spi_bus parent;
    spi_dev_t        *hw;
    spi_host_device_t host;
    rt_uint32_t       port;
    rt_uint32_t       freq;
};

static struct rt_s3_spi s_spi[RT_S3_SPI_NUM];

static spi_dev_t *rt_s3_spi_hw(rt_uint32_t port)
{
    return (port == 0) ? &GPSPI2 : &GPSPI3;
}

/* Route SCLK/MOSI/MISO/CS through the GPIO matrix. S3 pads are fully mux-able,
 * so no IO_MUX peripheral function is used - only PIN_FUNC_GPIO plus the matrix
 * signal indices. */
static rt_err_t rt_s3_spi_set_pins(rt_uint32_t port, const struct rt_s3_spi_pins *p)
{
    /* The S3 names its GPSPI signals after the FSPI flash lines the matrix
     * shares them with. Both controllers have their own index set. */
    static const struct
    {
        int sclk, mosi, miso, cs;
    } idx[RT_S3_SPI_NUM] =
    {
        { FSPICLK_OUT_IDX, FSPID_OUT_IDX, FSPIQ_IN_IDX,  FSPICS0_OUT_IDX },
        { SPI3_CLK_OUT_IDX, SPI3_D_OUT_IDX, SPI3_Q_IN_IDX, SPI3_CS0_OUT_IDX },
    };

    rt_uint32_t i;

    if (port >= RT_S3_SPI_NUM)
    {
        return -RT_EINVAL;
    }

    for (i = 0; i < 4; i++)
    {
        rt_base_t pin = (i == 0) ? p->sclk : (i == 1) ? p->mosi : (i == 2) ? p->miso : p->cs;

        if (pin < 0)
        {
            continue;
        }

        if (((SOC_GPIO_VALID_GPIO_MASK >> pin) & 1ULL) == 0)
        {
            return -RT_EINVAL;
        }

        gpio_ll_func_sel(&GPIO, (uint8_t)pin, PIN_FUNC_GPIO);
        gpio_ll_input_enable(&GPIO, (uint8_t)pin);
        gpio_ll_set_output_enable_ctrl(&GPIO, (uint8_t)pin, true, true);
    }

    esp_rom_gpio_connect_out_signal((uint32_t)p->sclk, idx[port].sclk, false, false);
    esp_rom_gpio_connect_out_signal((uint32_t)p->mosi, idx[port].mosi, false, false);
    esp_rom_gpio_connect_in_signal((uint32_t)p->miso, idx[port].miso, false);

    if (p->cs >= 0)
    {
        esp_rom_gpio_connect_out_signal((uint32_t)p->cs, idx[port].cs, false, false);
    }

    /* MISO is an input from the peripheral's point of view. */
    gpio_ll_output_disable(&GPIO, (uint8_t)p->miso);
    gpio_ll_input_enable(&GPIO, (uint8_t)p->miso);

    return RT_EOK;
}

static rt_err_t rt_s3_spi_configure(struct rt_spi_device *device,
                                    struct rt_spi_configuration *cfg)
{
    struct rt_s3_spi *bus = (struct rt_s3_spi *)device->bus;
    spi_dev_t *hw = bus->hw;
    rt_uint32_t freq = cfg->max_hz;

    if (freq == 0 || freq > 80000000u)
    {
        freq = (freq == 0) ? 1000000u : 80000000u;
    }

    /* CPOL/CPHA -> the hardware mode field, 0..3. RT-Thread encodes them in the
     * low two bits of cfg->mode, which happens to match. */
    spi_ll_master_set_mode(hw, (rt_uint8_t)(cfg->mode & (RT_SPI_CPOL | RT_SPI_CPHA)));

    /* Bit order is per-direction and independent of the mode bits. */
    if (cfg->mode & RT_SPI_MSB)
    {
        spi_ll_set_tx_lsbfirst(hw, false);
        spi_ll_set_rx_lsbfirst(hw, false);
    }
    else
    {
        spi_ll_set_tx_lsbfirst(hw, true);
        spi_ll_set_rx_lsbfirst(hw, true);
    }

    /* The divider is only meaningful relative to the clock the peripheral
     * actually sees, so ask instead of assuming. esp_clk_apb_freq() follows
     * DFS. */
    spi_ll_master_set_clock(hw, (int)esp_clk_apb_freq(), (int)freq, 128);

    bus->freq = freq;
    return RT_EOK;
}

/*
 * One hardware transaction. The LL caps a single transfer at
 * SOC_SPI_MAXIMUM_BUFFER_SIZE bytes (64), so the caller loops.
 *
 * The full-duplex behaviour is derived from the message: if both buffers are
 * given the hardware shifts simultaneously, which is what full duplex means and
 * what every SPI peripheral expects.
 */
static rt_uint32_t rt_s3_spi_xfer_chunk(struct rt_s3_spi *bus,
                                        const rt_uint8_t *tx, rt_uint8_t *rx,
                                        rt_uint32_t len, rt_bool_t keep_cs)
{
    spi_dev_t *hw = bus->hw;
    rt_uint32_t waited_us = 0;
    rt_uint32_t budget_us;

    spi_ll_cpu_tx_fifo_reset(hw);
    spi_ll_cpu_rx_fifo_reset(hw);

    spi_ll_set_command_bitlen(hw, 0);
    spi_ll_set_addr_bitlen(hw, 0);
    spi_ll_set_dummy(hw, 0);
    spi_ll_set_mosi_bitlen(hw, (tx != RT_NULL) ? len * 8u : 0u);
    spi_ll_set_miso_bitlen(hw, (rx != RT_NULL) ? len * 8u : 0u);

    spi_ll_enable_mosi(hw, (tx != RT_NULL) ? 1 : 0);
    spi_ll_enable_miso(hw, (rx != RT_NULL) ? 1 : 0);

    spi_ll_master_keep_cs(hw, keep_cs ? 1 : 0);

    if (tx != RT_NULL)
    {
        spi_ll_write_buffer(hw, tx, len * 8u);
    }

    /* A stale trans_done left over from a previous transfer would make the poll
     * below succeed immediately and then read an empty RX FIFO. */
    spi_ll_clear_intr(hw, SPI_LL_INTR_TRANS_DONE);

    spi_ll_apply_config(hw);
    spi_ll_user_start(hw);

    /*
     * Polled completion, with a deadline in microseconds rather than an
     * iteration count. A 64-byte transaction is 512 bit times: ~5 ms at 100 kHz,
     * ~64 us at 8 MHz. The bound is derived from the configured frequency so a
     * slow bus gets the time it needs, and a stuck peripheral still returns
     * instead of hanging the shell.
     */
    budget_us = (rt_uint32_t)(((rt_uint64_t)len * 8u * 1000000ull) / (bus->freq ? bus->freq : 1000000u));
    budget_us = budget_us * 4u + 20000u;
    if (budget_us > 500000u)
    {
        budget_us = 500000u;
    }

    while (!spi_ll_usr_is_done(hw))
    {
        if (waited_us >= budget_us)
        {
            rt_kprintf("spi%d: transfer timeout after %u us\n",
                       (int)bus->port, (unsigned)budget_us);
            spi_ll_cpu_tx_fifo_reset(hw);
            spi_ll_cpu_rx_fifo_reset(hw);
            return 0;
        }

        esp_rom_delay_us(10);
        waited_us += 10;
    }

    if (rx != RT_NULL)
    {
        spi_ll_read_buffer(hw, rx, len * 8u);
    }

    return len;
}

static rt_ssize_t rt_s3_spi_xfer(struct rt_spi_device *device,
                                 struct rt_spi_message *message)
{
    struct rt_s3_spi *bus = (struct rt_s3_spi *)device->bus;
    rt_uint32_t total = 0;

    if (message == RT_NULL)
    {
        return 0;
    }

    /* Take CS before the first chunk and release after the last: the loop below
     * has to keep CS asserted across chunks or the peripheral sees several
     * separate transactions. */
    if (message->cs_take)
    {
        spi_ll_master_select_cs(bus->hw, 0);
        spi_ll_master_keep_cs(bus->hw, 1);
    }

    while (total < message->length)
    {
        rt_uint32_t left = message->length - total;
        rt_uint32_t chunk = (left > SOC_SPI_MAXIMUM_BUFFER_SIZE)
                            ? SOC_SPI_MAXIMUM_BUFFER_SIZE : left;
        const rt_uint8_t *tx = (message->send_buf != RT_NULL)
                               ? ((const rt_uint8_t *)message->send_buf + total) : RT_NULL;
        rt_uint8_t *rx = (message->recv_buf != RT_NULL)
                         ? ((rt_uint8_t *)message->recv_buf + total) : RT_NULL;
        rt_bool_t last = (total + chunk) >= message->length;

        if (rt_s3_spi_xfer_chunk(bus, tx, rx, chunk, !last) == 0)
        {
            break;
        }

        total += chunk;
    }

    if (message->cs_release)
    {
        spi_ll_master_keep_cs(bus->hw, 0);
        spi_ll_master_select_cs(bus->hw, -1);
    }

    return (rt_ssize_t)total;
}

static const struct rt_spi_ops rt_s3_spi_ops =
{
    .configure = rt_s3_spi_configure,
    .xfer      = rt_s3_spi_xfer,
};

static rt_err_t rt_s3_spi_init_port(rt_uint32_t port, const struct rt_s3_spi_pins *pins,
                                    rt_uint32_t freq)
{
    struct rt_s3_spi *bus = &s_spi[port];
    char name[8];

    bus->hw   = rt_s3_spi_hw(port);
    bus->host = (port == 0) ? SPI2_HOST : SPI3_HOST;
    bus->port = port;

    if (rt_s3_spi_set_pins(port, pins) != RT_EOK)
    {
        return -RT_EINVAL;
    }

    /* Both of these are macros that require an RCC atomic environment in scope
     * (they reference __DECLARE_RCC_ATOMIC_ENV). PERIPH_RCC_ATOMIC() is the IDF
     * idiom and is what the esp_driver_spi path would use as well. */
    PERIPH_RCC_ATOMIC()
    {
        spi_ll_enable_bus_clock(bus->host, true);
        spi_ll_reset_register(bus->host);
        /* The bus clock alone is not enough: the master state machine runs on a
         * separate gate. Without it usr=1 starts a transaction that never raises
         * trans_done - a timeout with no other symptom. */
        spi_ll_enable_clock(bus->host, true);
    }

    spi_ll_master_init(bus->hw);

    /* One-line mode for all three phases. Without this the fcmd/faddr/fread bits
     * keep whatever the ROM left and the first transfer can go out as a
     * multi-line command. */
    {
        spi_line_mode_t line = { .cmd_lines = 1, .addr_lines = 1, .data_lines = 1 };

        spi_ll_master_set_line_mode(bus->hw, line);
    }

    /* Full duplex, MOSI/MISO both enabled. */
    spi_ll_set_half_duplex(bus->hw, false);

    bus->freq = freq ? freq : 1000000u;
    spi_ll_master_set_clock(bus->hw, (int)esp_clk_apb_freq(), (int)bus->freq, 128);

    bus->parent.mode = RT_SPI_BUS_MODE_SPI;

    name[0] = 's'; name[1] = 'p'; name[2] = 'i';
    name[3] = (char)('0' + port); name[4] = '\0';

    return rt_spi_bus_register(&bus->parent, name, &rt_s3_spi_ops);
}

/* ------------------------------------------------------------------ msh -- */

/*
 * Loopback self test: MOSI tied to MISO with a jumper.
 *
 * This is the only way to prove a SPI master works without a peripheral on the
 * bench, and it exercises the actual data path - FIFO, bit order, clock, both
 * directions - rather than just register state. Without the jumper it reports
 * all-zero reads, which is the expected result on an open bus and is stated as
 * such instead of being reported as a driver failure.
 */
static int cmd_spiloop(int argc, char **argv)
{
    rt_uint32_t port = 0;
    struct rt_spi_device *dev;
    struct rt_spi_configuration cfg;
    rt_uint8_t tx[8] = { 0xA5, 0x5A, 0x01, 0x80, 0xFF, 0x00, 0x3C, 0xC3 };
    rt_uint8_t rx[8] = { 0 };
    char bus_name[8];
    int i;

    if (argc >= 2)
    {
        port = (rt_uint32_t)atoi(argv[1]);
    }

    if (port >= RT_S3_SPI_NUM)
    {
        rt_kprintf("usage: spiloop [port 0..%d]\n", RT_S3_SPI_NUM - 1);
        return -1;
    }

    bus_name[0] = 's'; bus_name[1] = 'p'; bus_name[2] = 'i';
    bus_name[3] = (char)('0' + port); bus_name[4] = '\0';

    /*
     * rt_device_find("spi0") returns the BUS, not a device. Casting that pointer
     * to rt_spi_device and calling rt_spi_configure() dereferences a mutex that
     * was never initialised. A real rt_spi_device has to be attached to the bus
     * first: that call is what fills in device->bus, the CS pin and the lock.
     *
     * Attached once, and remembered. Attaching again on every invocation was
     * measured to fail - rt_spi_bus_attach_device() registers the name with
     * rt_device_register(), so the second call returns -RT_EEXIST and the
     * command printed "attach failed" instead of testing anything. In a 3-minute
     * soak that turned every call after the first into a no-op.
     */
    {
        static struct rt_spi_device loop_dev;
        static rt_bool_t attached = RT_FALSE;

        if (!attached)
        {
            if (rt_spi_bus_attach_device(&loop_dev, "spiloop", bus_name, RT_NULL) != RT_EOK)
            {
                rt_kprintf("spiloop: attach to %s failed\n", bus_name);
                return -1;
            }
            attached = RT_TRUE;
        }

        dev = &loop_dev;
    }

    rt_memset(&cfg, 0, sizeof(cfg));
    cfg.data_width = 8;
    cfg.mode       = RT_SPI_MASTER | RT_SPI_MODE_0 | RT_SPI_MSB;
    cfg.max_hz     = 1000000;

    if (rt_spi_configure(dev, &cfg) != RT_EOK)
    {
        rt_kprintf("spiloop: configure failed\n");
        return -1;
    }

    rt_memset(rx, 0, sizeof(rx));

    /*
     * Time the transfer in CPU cycles, not ticks.
     *
     * A 64-bit-time transaction at 1 MHz is ~64 us, and the kernel tick is 1 ms,
     * so rt_tick_get() reports 0 and tells the reader nothing. The cycle counter
     * is the only clock on this chip with the resolution to see the difference
     * between "the state machine ran" and "it returned immediately".
     *
     * On a board with no jumper the data check cannot pass, so this timing is
     * what still says something useful.
     */
    {
        rt_uint32_t c0, c1, hz;
        rt_ssize_t n;

        hz = esp_clk_cpu_freq();
        c0 = (rt_uint32_t)esp_cpu_get_cycle_count();
        n = rt_spi_transfer(dev, tx, rx, sizeof(tx));
        c1 = (rt_uint32_t)esp_cpu_get_cycle_count();

        if (n != (rt_ssize_t)sizeof(tx))
        {
            rt_kprintf("spiloop: transfer failed (%d)\n", (int)n);
            return -1;
        }

        rt_kprintf("spiloop: %s %u bytes, %u cycles = ~%u us "
                   "(expected ~%u us at %u Hz)\n",
                   bus_name, (unsigned)sizeof(tx), (unsigned)(c1 - c0),
                   (unsigned)((rt_uint64_t)(c1 - c0) * 1000000ull / hz),
                   (unsigned)(sizeof(tx) * 8u * 1000000u / cfg.max_hz),
                   (unsigned)cfg.max_hz);
    }

    rt_kprintf("spiloop: %s tx -> rx:", bus_name);
    for (i = 0; i < (int)sizeof(tx); i++)
    {
        rt_kprintf(" %02x", rx[i]);
    }
    rt_kprintf("\n");

    if (rt_memcmp(tx, rx, sizeof(tx)) == 0)
    {
        rt_kprintf("spiloop: PASS - MOSI and MISO are jumpered and agree\n");
    }
    else
    {
        /* Distinguish "open bus" from "wrong data": all zeros means nothing was
         * driving MISO, which is what an unjumpered board looks like. */
        rt_bool_t all_zero = RT_TRUE;

        for (i = 0; i < (int)sizeof(rx); i++)
        {
            if (rx[i] != 0)
            {
                all_zero = RT_FALSE;
            }
        }

        if (all_zero)
        {
            rt_kprintf("spiloop: no loopback - tie MOSI to MISO with a jumper "
                       "to run this test\n");
        }
        else
        {
            rt_kprintf("spiloop: FAIL - data came back but does not match\n");
        }
    }

    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_spiloop, spiloop, SPI loopback test (MOSI->MISO jumper));

rt_err_t rt_hw_spi_init(void)
{
    /* GPIO11/12/13/10: the S3's IO_MUX default FSPI pins for SCLK/MOSI/MISO/CS,
     * and all four are free on this board (not flash, not PSRAM, not the LED, not
     * a strapping pin). Using the defaults means a device wired to a standard
     * ESP32-S3 SPI header works without reconfiguration. */
    static const struct rt_s3_spi_pins pins0 = { .sclk = 12, .mosi = 11, .miso = 13, .cs = 10 };

    rt_err_t rc = rt_s3_spi_init_port(0, &pins0, 1000000u);

    if (rc != RT_EOK)
    {
        rt_kprintf("spi0: init failed %d\n", rc);
    }

    return rc;
}
INIT_BOARD_EXPORT(rt_hw_spi_init);

#endif /* SOC_GPSPI_SUPPORTED */
