# RT-Thread component

Run RT-Thread instead of FreeRTOS on ESP32-C3, ESP32-C5, ESP32-C6, ESP32-S3 and
ESP32-S31 with ESP-IDF v6.1. The kernel comes from an unmodified upstream
RT-Thread checkout; the CPU port, `rtconfig.h` and board init live in this
component.

## Supported chips

| Chip | Arch | Status |
| --- | --- | --- |
| ESP32-C3 | RISC-V RV32IMC | Tested on a board: boots into the demo threads, passes a 3 minute flash erase/write stress test |
| ESP32-C5 | RISC-V RV32IMAC (CLIC) | **Tested on a board** (ESP32-C5HR8 rev v1.2, 240 MHz, 16 MB flash, 8 MB in-package PSRAM, 48 MHz crystal): 1 kHz tick, two preempting threads, msh on UART0, `pin` device with GPIO interrupts, flash erase/write across the cache-off window |
| ESP32-C6 | RISC-V RV32 | Builds and links, not tested on a board yet |
| ESP32-S3 | Xtensa LX7 | **Tested on a board** (立创 ESP32S3R8N8, 240 MHz, 8 MB flash + 8 MB octal PSRAM at 80 MHz): 1 kHz tick verified over 4 minutes, two threads preempting, FPU round-trip clean, msh on UART0 with interrupt-driven RX, PSRAM pattern test, GPIO + board LED + button interrupt, LEDC PWM, I2C bus, SPI bus and SAR ADC |
| ESP32-S31 | RISC-V (CLIC) | Tested on a board, dual core, PSRAM/Wi-Fi/BLE |

Single core on C3/C5/C6/S3; the S31 port runs both harts. Wi-Fi and Bluetooth are
supported only on S31 so far.

On ESP32-C5 the port provides two devices so far. The rest of the S31 driver set
is deliberately absent: its LL headers and pin maps differ, and a copied driver
that compiles is not a driver that works.

| Device | Driver | Notes |
| --- | --- | --- |
| `uart0` | `serial.c` | rt_serial v1, interrupt-driven RX, console output through the ROM |
| `pin` | `gpio.c` | GPIO in/out, open drain, per-pin interrupts via `esp_intr_alloc()` |

The C5 has no FPU (`rv32imac_zicsr_zifencei_zaamo_zalrsc`), so the trap frame has
no FP area and `ARCH_RISCV_FPU` stays undefined.

On ESP32-S3 the port provides these RT-Thread devices, all over the LL layer
rather than `esp_driver_*` - those call FreeRTOS blocking APIs:

| Device | Driver | Notes |
| --- | --- | --- |
| `uart0` | `serial.c` | rt_serial v1, interrupt-driven RX on top of `console.c`'s ring |
| `pin` | `gpio.c` | GPIO in/out, open drain, per-pin interrupts via `esp_intr_alloc()` |
| `pwm` | `pwm.c` | LEDC, 8 channels / 4 timers, XTAL clock source |
| `i2c0` | `i2c.c` | master, 100 kHz / 400 kHz, command-list FSM |
| `spi0` | `spi.c` | GPSPI2 master, polled, 64-byte chunks |
| `adc1`, `adc2` | `adc.c` | SAR oneshot, 12-bit raw code (no voltage calibration) |

## Enable

`idf.py menuconfig` → `Component config` → `FreeRTOS` → `Extra` →
`Replace FreeRTOS with RT-Thread`, or in `sdkconfig.defaults`:

```
CONFIG_IDF_RTOS_RTTHREAD=y
CONFIG_FREERTOS_HZ=1000
```

`CONFIG_FREERTOS_HZ` must match `RT_TICK_PER_SECOND` (1000). On S3 and C5 also
set `CONFIG_FREERTOS_UNICORE=y` and `CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE=y`; the
two have to agree or `app_startup.c` fails the build, and `startup.c` #errors if
IDF brings up more cores than `RT_CPUS_NR`.

## Build

```sh
git clone https://github.com/RT-Thread/rt-thread.git
export RTTHREAD_ROOT=$PWD/rt-thread
cd your_project
idf.py set-target esp32s3   # or esp32c3 / esp32c5 / esp32c6 / esp32s31
idf.py build flash monitor
```

`RTTHREAD_ROOT` can also be passed as `-DRTTHREAD_ROOT=<path>`. The port supplies
`rt_hw_console_output()` on every target, so `rt_kprintf()` works out of the box.

## How the Xtensa port is put together

The S3 port is deliberately much smaller than the RISC-V ones, because it does
not reimplement anything ESP-IDF already has:

| Layer | Where it comes from |
| --- | --- |
| Vector table, window overflow/underflow, level 2..6 and debug/NMI vectors, `_xt_panic`, coprocessor exception path, `_xt_context_save`/`_restore` | `components/xtensa/xtensa_vectors.S` and `xtensa_context.S`, **unmodified** |
| The five RTOS callouts those files need | `port/esp32s3/context_gcc.S` (`_rtt_int_enter`, `_rtt_int_exit`, `_rtt_resume`, `_rtt_coproc_state`, `_rtt_coproc_exc_hook`) |
| `rt_hw_context_switch*`, `rt_hw_interrupt_disable/enable` | same file |
| `rt_hw_stack_init`, ISR stack, FPU policy | `port/esp32s3/cpuport.c` |
| `rt_hw_interrupt_install/mask/umask` | `port/esp32s3/interrupt.c`, onto `xt_set_interrupt_handler()` |
| `xPortEnterCriticalTimeout` / `vPortExitCritical` / `xPortInIsrContext` (which IDF components call directly) | `port/esp32s3/critical.c` |
| Tick | IDF's SYSTIMER alarm → `SysTickIsrHandler` → `rt_tick_increase()` |
| Console | `port/esp32s3/console.c` - ROM writer for output, UART0 RX interrupt for input |
| `uart0` / `pin` / `pwm` / `i2c0` / `spi0` / `adc1`+`adc2` | `port/esp32s3/{serial,gpio,pwm,i2c,spi,adc}.c` over the UART/GPIO/LEDC/I2C/GPSPI/ADC LL |

`xtensa_vectors.S` names no RTOS symbol at all: it reaches the OS only through
those macros. That is what makes this reuse possible, and it is why the port does
**not** carry its own vector table (the alternative, taken by the only existing
S3 fork, needs a hand-maintained 31 KB IDF source list and reimplements the
vectors).

Because the Xtensa FreeRTOS port's `portasm.S`/`port.c` define the same symbols
(`_xt_user_exit`, the `_frxt_*` dispatch, the coprocessor path), they are removed
from the build on this target - see `components/freertos/CMakeLists.txt`. The
pieces of the port API that IDF code calls directly are reimplemented in
`port/esp32s3/critical.c` instead.

### FPU

The S3's single-precision FPU is coprocessor 0. `f0`-`f15` are *caller* saved in
the windowed ABI, so:

- a cooperative switch needs no FP save (GCC already spilled),
- an interrupt does: `_rtt_int_enter` saves `FCR`/`FSR`/`f0`-`f15` into an 80-byte
  block below the `XtExcFrame`, and `thread->sp` points at that block.

`CPENABLE` keeps CP0 enabled for thread code and is cleared while C ISR code runs.
This is why the port needs none of IDF's lazy coprocessor-ownership machinery.
AI/SIMD (CP3) stays disabled; an `ee.*` instruction panics on purpose.

## Notes

- Do not call the RT-Thread scheduler while the flash cache is disabled. The
  RISC-V port asserts on it; the Xtensa port places `context_gcc.S`,
  `interrupt.c` and `cpuport.c` in IRAM and leaves the rest to the caller.
- With `CONFIG_IDF_RTOS_RTTHREAD=n` this component is empty and the build is the
  same as stock ESP-IDF.

## How the ESP32-C5 port is put together

The C5 is a CLIC part like the S31 and a one-core part like the C6, so it is
built from the C6's pieces - `port/esp32c5` differs from `port/esp32c6` in the
SoC name, in the two trap-path tables below, and in the drivers it ships.

| Layer | Where it comes from |
| --- | --- |
| Trap entry, threshold save/raise/restore, IRQ-context nesting | `components/riscv/vectors_rtthread.S`, shared and unmodified |
| `rt_hw_stack_init`, `rt_hw_interrupt_is_disabled`, `rt_irq_nest_slot`, `rt_irq_stack_top` | `port/esp32c5/cpuport.c` |
| `rt_hw_interrupt_install/mask/umask`, `rt_hw_intr_route`, the CLIC IE bit | `port/esp32c5/interrupt.c` |
| `uart0` + `rt_hw_console_output()` | `port/esp32c5/serial.c` over the UART LL and the ROM writer |
| `pin` | `port/esp32c5/gpio.c` over the GPIO LL |
| Tick | IDF's SYSTIMER alarm -> `SysTickIsrHandler` -> `rt_tick_increase()` |
| Interrupt stack and heap | `components/esp_system/ld/esp32c5/sections.ld.in` |

Three things about this target are worth knowing before changing it:

1. **The two trap tables are `.data`, not `.rodata`.** `vectors_rtthread.S` indexes
   `rt_irq_nest_slot` and `rt_irq_stack_top` by `mhartid` on every trap, so a
   one-core build still has to provide both arrays. With `const` pointers the
   compiler is free to put them in `.rodata` (flash), and the trap handler reads
   them while the cache can be off.

2. **The C5 masks a line by writing CLIC INTIE directly.** On this SoC the linker
   maps `esprv_int_enable`/`esprv_int_disable` onto the ROM's old INTC
   implementations, which do not touch CLIC INTIE - so masking through them is a
   no-op that no amount of "the handler installed fine" output reveals. Found on
   the S31, whose ROM layout is the same.

3. **GPIO interrupts take their line number from `esp_intr_alloc()`.** Line 4 is
   not "the GPIO line": it is whichever line the allocator handed out, and on
   this image that is the OS tick. The S31 port lost three rounds to that
   assumption; `port/esp32c5/gpio.c` allocates by source instead.

