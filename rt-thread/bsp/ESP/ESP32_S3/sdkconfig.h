/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-11-03     RT-Thread    first version, ESP32-S3 (Xtensa LX7) port
 *
 * sdkconfig.h for the scons build; a no-op when ESP-IDF already provides one.
 *
 * The board drivers in this directory include "sdkconfig.h" for a handful of
 * values (UART baud rate, crystal frequency, whether the build is unicore).
 * Under ESP-IDF that name resolves to the generated header in build/config;
 * under scons nothing generates one, so this file supplies the same names from
 * rtconfig.h.
 *
 * The two cannot coexist: quotes in an #include search the including file's own
 * directory FIRST, so this file always wins for the drivers here, and then
 * IDF's generated sdkconfig.h gets included later through a different path and
 * every overlapping macro is redefined - measured as
 *
 *   board/sdkconfig.h:38:9: error: 'CONFIG_FREERTOS_HZ' redefined
 *
 * The guard below is the fix, and it is the standard idiom: ESP-IDF's generated
 * header always defines CONFIG_IDF_TARGET_ESP32S3, so in an IDF build this file
 * expands to nothing and the real header is the only one seen.
 *
 * It deliberately contains only what these drivers read. A broader copy would be
 * a second, silently-stale version of IDF's configuration.
 */
#if !defined(CONFIG_IDF_TARGET_ESP32S3)

#ifndef __SDKCONFIG_SHIM_H__
#define __SDKCONFIG_SHIM_H__

#include "rtconfig.h"

/*
 * The target macro, in the exact form IDF generates it: a *string*, because
 * IDF stringises it into deprecation messages and attribute text. Measured
 * without it:
 *
 *   periph_ctrl.h:87:102: error: expected ')' before 'CONFIG_IDF_TARGET'
 *
 * which is the `deprecated("... on " CONFIG_IDF_TARGET)` attribute collapsing
 * when the macro is undefined. Defining it as a bare token instead of a string
 * fails the same way one level further down.
 */
#ifndef CONFIG_IDF_TARGET
#define CONFIG_IDF_TARGET                   "esp32s3"
#endif
#ifndef CONFIG_IDF_TARGET_ESP32S3
#define CONFIG_IDF_TARGET_ESP32S3           1
#endif

/* The kernel is built by RT-Thread, not FreeRTOS, and only one core is
 * released; board.c relies on both being true when it decides what to
 * initialise. */
#ifndef CONFIG_IDF_RTOS_RTTHREAD
#define CONFIG_IDF_RTOS_RTTHREAD            1
#endif
#ifndef CONFIG_FREERTOS_UNICORE
#define CONFIG_FREERTOS_UNICORE             1
#endif
#ifndef CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE
#define CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE  1
#endif

/* Must match RT_TICK_PER_SECOND: the tick comes from the system timer alarm and
 * port_systick.c asserts on the equality. */
#ifndef CONFIG_FREERTOS_HZ
#define CONFIG_FREERTOS_HZ                  RT_TICK_PER_SECOND
#endif

/* Console baud rate. The ROM bootloader prints at this rate and the CH340K on
 * this board is wired to UART0, so nothing is negotiated. */
#ifndef CONFIG_ESP_CONSOLE_UART_BAUDRATE
#define CONFIG_ESP_CONSOLE_UART_BAUDRATE    115200
#endif

/* Crystal, in MHz. board.c does not program the PLL (the chip's startup code
 * does), but the tick conversion and the LEDC divider both need the real
 * value. */
#ifndef CONFIG_XTAL_FREQ
#define CONFIG_XTAL_FREQ                    40
#endif

/* Port critical sections may not suspend the scheduler; critical.c picks the
 * cheaper of two implementations based on this. */
#ifndef CONFIG_FREERTOS_CHECK_PORT_CRITICAL_COMPLIANCE
#define CONFIG_FREERTOS_CHECK_PORT_CRITICAL_COMPLIANCE 0
#endif

/*
 * Newlib, not picolibc.
 *
 * ESP-IDF 6.1 moved the C-library compatibility shims into esp_libc, and they
 * are only switched on when CONFIG_LIBC_NEWLIB is set. Without it esp_libc
 * declares an empty `struct _reent`, while the toolchain's <stdio.h> expands
 * `stdin` into `_REENT->_stdin` - which then fails to compile with
 *
 *   stdio.h:66:9: error: unknown type name '__FILE'
 *   stdio.h:696:33: error: request for member '_w' in something not a structure
 *   stdio.h:750:16: error: '_REENT' undeclared
 *
 * The scons build links against the toolchain's own newlib (the xtensa-esp-elf
 * default multilib does not ship picolibc), so newlib is the correct answer
 * here. The C3 BSP carries the same define for the same reason.
 */
#ifndef CONFIG_LIBC_NEWLIB
#define CONFIG_LIBC_NEWLIB                  1
#endif


#endif /* __SDKCONFIG_SHIM_H__ */

#endif /* !CONFIG_IDF_TARGET_ESP32S3 */
