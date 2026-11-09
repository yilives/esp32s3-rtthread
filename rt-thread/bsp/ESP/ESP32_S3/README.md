# ESP32-S3 (Xtensa LX7) BSP

RT-Thread on the ESP32-S3. Verified on a 立创 ESP32S3R8N8 board (8 MB flash,
8 MB octal PSRAM, 40 MHz crystal, 240 MHz CPU).

## What is where

The port is split the way the tree expects it to be:

| Layer | Location | Contents |
| --- | --- | --- |
| architecture | `libcpu/xtensa` | context switch, first context of a thread, FPU policy, backtrace |
| board | this directory (`board/`) | console, uart0, GPIO, LEDC, I2C, SPI, SAR ADC, the FreeRTOS-compatibility critical sections |

Both are compiled by the ESP-IDF component in
`esp-idf-rtt/components/rtthread`, which points at these directories rather than
keeping a copy. Nothing is duplicated.

The interrupt/exception vector table is **not** in this port: it is Espressif's
`xtensa_vectors.S`, which is OS agnostic and calls back through five macros.
See `libcpu/xtensa/README.md`.

## Two build routes

### 1. ESP-IDF (verified on hardware)

```sh
idf.py -DIDF_TARGET=esp32s3 -DCONFIG_IDF_RTOS_RTTHREAD=y build
idf.py -p /dev/ttyUSB0 flash monitor
```

The `rtthread` component in `esp-idf-rtt/components/rtthread` compiles:

- the kernel from the RT-Thread checkout (`src/`, `components/`)
- `libcpu/xtensa`
- this BSP's `board/`

IDF keeps the bootloader, the partition table and the flash layout.

### 2. scons (the plain RT-Thread flow)

```sh
scons
```

This matches how every other BSP in `bsp/` builds, and it needs two things that
are not in the tree:

- the **RT-Thread env tool**, for `./menuconfig` and `pkgs --update` (the same
  requirement the ESP32-C3 and ESP32-C6 BSPs have)
- an **ESP-IDF checkout** on the include path, for the chip's `xtensa-config`
  headers and the ROM/peripheral definitions:
  `export IDF_PATH=/path/to/esp-idf`

`sdkconfig.h` in this directory is a shim that supplies the handful of
`CONFIG_*` values the board drivers read when ESP-IDF is not generating one. It
compiles to nothing under the IDF build (`CONFIG_IDF_TARGET_ESP32S3` is defined
there), so the two routes can share the same sources.

Route 2 does not produce a flashable image yet - there is no bootloader step -
so it is the bring-up/debug flow, and route 1 is the one that has been run on a
board.

## Devices

| Device | Driver | Hardware |
| --- | --- | --- |
| `uart0` | `board/serial.c` | UART0 (GPIO43 TX / GPIO44 RX), interrupt-driven RX |
| `pin` | `board/gpio.c` | GPIO 0..48, open drain, per-pin interrupts |
| `pwm` | `board/pwm.c` | LEDC, 8 channels / 4 timers, XTAL clock |
| `i2c0` | `board/i2c.c` | master, 100/400 kHz |
| `spi0` | `board/spi.c` | GPSPI2 master, polled |
| `adc1`, `adc2` | `board/adc.c` | SAR oneshot, 12-bit raw code |

Shell commands: `s3rt ...` (board report, tick, PSRAM, LED, PWM, button),
`i2cscan`, `spiloop`, `s3adc`, `xtbt` (Xtensa backtrace).

## Wireless

Wi-Fi and BLE are **IDF's stacks, unmodified**, running on RT-Thread threads
through the FreeRTOS compatibility layer in `components/rtthread/port/freertos/`.
Nothing about the radio is architecture specific - xtensa vs riscv only decides
who saves the registers - so the BSP contributes no wireless code of its own.

Two things in this directory exist solely because of it, and both are easy to
get wrong:

| Hook | Why |
| --- | --- |
| `rt_hw_esp_timer_init()` in `board.c`, registered as `ESP_SYSTEM_INIT_FN(..., SECONDARY, BIT(0), 99)` | `esp_phy`, `esp_wifi` and `esp_coex` all create `esp_timer` callbacks, and Wi-Fi's scan-complete notification is delivered through one. IDF's own init function is compiled out under `CONFIG_IDF_RTOS_RTTHREAD`, so the BSP has to start it. |
| `INIT_COMPONENT_EXPORT(rt_hw_start_pending_tasks)` in `board.c` | Tasks created during the SECONDARY stage run before the RT-Thread scheduler exists, so `ft_task_create()` defers their startup. Without this flush they stay in `init` forever - `list thread` shows `esp_timer 8 init` - and every timer callback silently never fires. |

`RT_DEBUGING_CONTEXT` is deliberately **not** enabled in `rtconfig.h`:
`_rt_sem_take()` evaluates `RT_DEBUG_SCHEDULER_AVAILABLE(1)` before it looks at
the timeout, so even `rt_sem_trytake()` trips it, and `esp_coex`'s SECONDARY-stage
init function takes a semaphore before any scheduler exists. With it on the board
resets every boot at 877 ms. The reasoning and the measured log are in the
`rtconfig.h` header comment.

The commands live in the validation project (`s3-rtt-test/main/`), not here,
because they need the whole Wi-Fi/BLE stack configured in:

| Command | What it proves |
| --- | --- |
| `s3wifi [ssid] [pass]` | interface up, association, 4-way handshake, DHCP lease |
| `s3wifiscan` | the radio receives; lists 2.4 GHz APs with channel and RSSI |
| `s3net [host] [port]` | DNS + TCP connect + send + recv, i.e. the stack moves bytes |
| `s3ble` | BLE controller up, NimBLE host synced, own address read back |

"Got an IP" and "can talk to another host" are different claims; `s3net` is what
separates them.

## Board specifics

| Item | Value | Where it comes from |
| --- | --- | --- |
| LED | GPIO48, active low | schematic, LED block |
| BOOT button | GPIO0, pulled up, falling edge | schematic, button block |
| Console | CH340K on UART0, 115200 8N1 | schematic, USB block |
| Flash | 8 MB, QIO @ 80 MHz | measured with esptool |
| PSRAM | 8 MB octal @ 80 MHz | measured at runtime (`s3rt psram`) |

Pin conflicts the drivers warn about rather than silently mishandle:

| Pins | Used by | Also |
| --- | --- | --- |
| GPIO8, GPIO9 | `i2c0` SDA/SCL | ADC1 ch7/ch8 |
| GPIO10-13 | `spi0` CS/SCLK/MOSI/MISO | ADC1 ch9, ADC2 ch0-2 |
| GPIO19, GPIO20 | native USB | ADC2 ch8/ch9 |

The internal pull-ups are weak (~45 kOhm). At 100 kHz the I2C ACK sampling is
marginal on an empty bus, which `i2cscan` reports as `flaky`; adding external
4.7 kOhm pull-ups fixes it, and 20 kHz is clean without them.
