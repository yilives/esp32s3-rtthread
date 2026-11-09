/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-11-03     RT-Thread    first version, ESP32-S3 (Xtensa LX7) port
 */

/*
 * ESP32-S3 kernel RT-Thread configuration.
 *
 * The upstream RT-Thread checkout supplies src/ and include/ unmodified, so this
 * component owns the target configuration. Do not add kernel headers to this
 * directory - rtconfig.h must remain the only header here, so upstream headers
 * are not shadowed.
 *
 * Deliberate differences from port/esp32s31/rtconfig.h:
 *
 *   - RT_USING_SMP is ON, as it is on the S31. Both chips are dual core and both
 *     ports run the MP scheduler. What differs is the mechanism: S31 uses
 *     RISC-V's ticket lock (amoadd.w) on the shared CLIC; S3 uses Xtensa's
 *     S32C1I store-conditional and the four SYSTEM_CPU_INTR_FROM_CPU_n lines.
 *
 *   - ARCH_XTENSA rather than ARCH_RISCV.
 *
 * The console is the one place where this port carries two paths on purpose:
 * rt_hw_console_output()/rt_hw_console_getchar() (ROM writer + UART0 RX
 * interrupt) always exist, and once the serial device is registered the kernel
 * prefers it. See port/esp32s3/console.c and port/esp32s3/serial.c.
 */
#ifndef RT_CONFIG_H__
#define RT_CONFIG_H__

/* ESP32-S3: Xtensa LX7, windowed ABI, 64 physical address registers,
 * single-precision FPU (CP0), 32 interrupts in 6 levels, XEA2. */
#define ARCH_XTENSA
#define SOC_ESPRESSIF
#define SOC_ESP32_S3

/* The Xtensa windowed ABI spills in 16-byte units and every frame this port
 * builds (XtExcFrame, XtSolFrame, the FP save block) is 16-byte based. */
#define RT_NAME_MAX                     12
#define RT_ALIGN_SIZE                   16
/* Dual core, and this is the shipped configuration.
 *
 * RT_USING_SMP here means the kernel is src/cpu_mp.c + src/scheduler_mp.c, and
 * s3-rtt-test/sdkconfig.defaults clears CONFIG_FREERTOS_UNICORE and
 * CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE so IDF releases the second core.
 *
 * Both cores are live and everything works: 5/5 cold boots reach the shell,
 * `s3smp` reports cpu0 on tidle0 and cpu1 on tshell, and Wi-Fi + DHCP + TCP and
 * BLE all pass with both cores running (`s3rt` selftest also PASSes).
 *
 * The last thing that had to be fixed was board/critical.c: its IDF
 * portENTER_CRITICAL shim called rt_hw_interrupt_disable, which under
 * RT_USING_SMP is a *macro for rt_cpus_lock* - so every IDF critical section
 * contended on the single global _cpus_lock and a core could spin forever with
 * its interrupts already masked. It now uses rt_hw_local_irq_disable, which
 * masks this core only, matching what portENTER_CRITICAL actually promises.
 *
 * To go back to single core: comment the two defines below and restore
 * CONFIG_FREERTOS_UNICORE=y / CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE=y in
 * s3-rtt-test/sdkconfig.defaults. That path is tested too (QEMU gate PASS). */
#define RT_USING_SMP
#define RT_CPUS_NR                      2
#define RT_THREAD_PRIORITY_32
#define RT_THREAD_PRIORITY_MAX          32
/* Must equal CONFIG_FREERTOS_HZ: the tick comes from the IDF SYSTIMER alarm and
 * xPortSysTickHandler() calls rt_tick_increase() once per alarm. port_systick.c
 * has a _Static_assert on this. */
#define RT_TICK_PER_SECOND              1000

/* Kernel threads. The Xtensa context is large: XT_STK_FRMSZ is 192 bytes and the
 * FP save block another 80, so cpuport.c has a build-time check
 * (RT_S3_STACK_MIN) that these are large enough instead of letting the initial
 * frame run below the thread's own stack. */
#define IDLE_THREAD_STACK_SIZE          2048

/* Real system heap: kservice.c -> rt_smem_init() in mem.c. The memory region
 * itself comes from IDF_RTTHREAD_HEAP_SIZE (__heap_start__/__heap_end__ in
 * esp_system/ld/esp32s3/sections.ld.in). */
#define RT_USING_HEAP
#define RT_USING_SMALL_MEM
#define RT_USING_SMALL_MEM_AS_HEAP

#define RT_USING_SEMAPHORE
#define RT_USING_MUTEX
#define RT_USING_EVENT

/* Device framework and the serial v1 device.
 *
 * RT_USING_DEVICE is what makes `list device` exist and what lets finsh take its
 * characters from an rt_device instead of from rt_hw_console_getchar(). The
 * serial driver in port/esp32s3/serial.c registers uart0 through
 * rt_hw_serial_register() and rt_console_set_device() makes it the console, so
 * rt_kprintf goes through the device stack once it is up - while
 * rt_hw_console_output() stays as the ROM-based fallback for the window before
 * that and for panic paths.
 *
 * SERIAL_V1 rather than V2: V2 needs RT_USING_DM and a device tree, and this
 * port has neither. dev_serial.c is also what every other ESP port here uses. */
#define RT_USING_DEVICE
#define RT_USING_SERIAL
#define RT_USING_SERIAL_V1
#define RT_SERIAL_RB_BUFSZ              128

/* The pin device. rt_pin_*() in rtdevice.h only exists with this on, and
 * port/esp32s3/gpio.c registers the ops through rt_device_pin_register(). */
#define RT_USING_PIN

/* PWM. rt_device_pwm / rt_pwm_set only exist with this on, and port/esp32s3/
 * pwm.c registers through rt_device_pwm_register(). */
#define RT_USING_PWM

/* I2C. rt_i2c_bus_device_register / rt_i2c_transfer only exist with this on,
 * and port/esp32s3/i2c.c registers through it.
 *
 * Deliberately NOT RT_USING_I2C_BITOPS: this driver talks to the I2C
 * peripheral's own state machine, not to two GPIOs, and enabling the bit-bang
 * implementation would compile a second, unrelated bus driver. */
#define RT_USING_I2C

/* SPI. rt_spi_bus_register / rt_spi_transfer only exist with this on, and
 * port/esp32s3/spi.c registers through it.
 *
 * Deliberately NOT RT_USING_SPI_MSD / RT_USING_SFUD: those need the DFS layer
 * and a block device, neither of which this image has. */
#define RT_USING_SPI

/* ADC. The v1 framework (adc.c) is chosen over v2: v2 needs RT_USING_DM and a
 * device tree, and this port has neither. port/esp32s3/adc.c registers through
 * rt_hw_adc_register(), which is the v1 API. */
#define RT_USING_ADC

/* Console and shell. FINSH_USING_SYMTAB needs __fsymtab_start/__fsymtab_end and
 * the .rti_fn* table; both are emitted by esp_system/ld/ld.flash.sections, which
 * every target shares, so nothing target specific is required here. */
#define RT_USING_CONSOLE
#define RT_CONSOLEBUF_SIZE              256
/* INIT_*_EXPORT() expands to nothing without this, which would leave the
 * .rti_fn table empty and stop finsh_system_init() from ever running. */
#define RT_USING_COMPONENTS_INIT
#define RT_USING_FINSH
#define FINSH_USING_MSH
/* ps / free / list live behind this switch in msh.c. Without it the shell still
 * works and `help` still answers, it just silently loses the commands a
 * bring-up image actually needs. */
#define MSH_USING_BUILT_IN_COMMANDS
#define FINSH_THREAD_NAME               "tshell"
#define FINSH_THREAD_PRIORITY           20
#define FINSH_THREAD_STACK_SIZE         4096
#define FINSH_USING_HISTORY
#define FINSH_HISTORY_LINES             5
#define FINSH_USING_SYMTAB
#define FINSH_USING_DESCRIPTION
#define FINSH_CMD_SIZE                  80
#define FINSH_ARG_MAX                   8

/* kservice.c compiles the generic backtrace walker unconditionally. */
#define RT_BACKTRACE_LEVEL_MAX_NR       8

/* Still undefined on purpose, not by accident. Each of these needs a component
 * this image does not link, so defining it would compile code whose
 * dependencies are absent rather than add a feature:
 *
 *   RT_USING_SOCKET / RT_USING_LIBC   - the lwIP port and newlib are not wired in
 *   RT_USING_DFS                      - no filesystem, so no /dev and no ymodem
 *   RT_USING_SMART                    - that is the microkernel variant
 *   RT_USING_SMP                      - now ON, see the header comment; RT_USING_HW_ATOMIC
 *                                       stays off because Xtensa's atomics are the
 *                                       S32C1I store-conditional, not a compare-and-swap
 *                                       on a plain word, and the kernel's software
 *                                       fallback (disable interrupts) is wrong once two
 *                                       cores can run at the same time
 *   RT_USING_HW_ATOMIC                - NOW ON, and it is mandatory under SMP.
 *
 * The software fallback in include/rtatomic.h implements every atomic by
 * masking interrupts with rt_hw_interrupt_disable(). Under RT_USING_SMP that
 * name is a macro onto rt_cpus_lock() (include/rthw.h:146), and
 * rt_cpus_lock() itself calls rt_atomic_load() (src/cpu_mp.c:177). The two
 * recurse into each other without a base case:
 *
 *     rt_cpus_lock -> rt_atomic_load -> rt_soft_atomic_load
 *       -> rt_hw_interrupt_disable (== rt_cpus_lock) -> ...
 *
 * Measured on hardware: the tick interrupt walks the stack down through the
 * 4 KB ISR stack, through the neighbouring BSS, into rt_xt_core[], and off the
 * bottom of DRAM (sp = 0x3fc80020, below 0x3FC88000). The double exception that
 * follows prints nothing, which is why the dual-core bring-up looked "hung".
 *
 * libcpu/xtensa/atomic_xtensa.c implements the whole rt_hw_atomic_* set with
 * S32C1I (the ISA's store-conditional) plus plain aligned load/store, so the
 * recursion is gone at the root.
 */
#define RT_USING_HW_ATOMIC

/* ---------------------------------------------------------------------------
 * The rest of the upstream kernel defaults, enabled.
 *
 * Everything above was chosen by what the bring-up image needed; everything
 * below is what a stock `RT-Thread Kconfig` configuration has by default
 * (src/Kconfig: RT_USING_DEBUG y, RT_USING_HOOK y, RT_USING_TIMER_SOFT y,
 * RT_USING_MEMPOOL y, RT_USING_MAILBOX y, RT_USING_MESSAGEQUEUE y). Leaving
 * them off made this port a minimal kernel rather than a normal one: code that
 * any ordinary RT-Thread package expects to link - rt_mb_*, rt_mq_*, rt_mp_*,
 * rt_thread_idle_sethook() - was simply absent.
 *
 * They were verified together, not one at a time, because several interact:
 *
 *   - RT_USING_TIMER_SOFT starts the `timer` thread. Measured on the board:
 *     `list thread` gains exactly one row (`timer`, priority 4, 13% stack).
 *   - RT_USING_DEBUG + RT_DEBUGING_ASSERT makes RT_ASSERT real instead of
 *     `{RT_UNUSED(EX);}`. rt_assert_handler() prints and parks in a loop, which
 *     is the behaviour a reader of the source expects; before this, every
 *     RT_ASSERT in the kernel and in the component drivers was compiled away -
 *     verified with nm: rt_assert_handler was not in the image at all.
 *   - RT_USING_OVERFLOW_CHECK adds rt_scheduler_stack_check(), which looks for
 *     the '#' fill pattern thread.c paints at the stack's low end. Xtensa grows
 *     downward and ARCH_CPU_STACK_GROWSDOWN is the default, so the check reads
 *     the right end without any target-specific code.
 *
 * Measured with all of them on: 185 commands over 200 s across every device,
 * 0 panics and 0 assertions, tick still exact. The heap is unchanged (65440
 * bytes) because the timer thread's stack comes from the heap at init, and the
 * idle thread is the only other addition.
 *
 * RT_DEBUGING_CONTEXT is the one upstream default deliberately NOT enabled, and
 * the reason is measured rather than stylistic:
 *
 *   It turns on RT_DEBUG_IN_THREAD_CONTEXT, which _rt_sem_take() evaluates
 *   UNCONDITIONALLY at src/ipc.c:574 - before looking at the timeout, so
 *   rt_sem_trytake() hits it too. The check is "is there a current thread",
 *   i.e. "has the scheduler started". IDF's SECONDARY-stage init functions run
 *   before that, by design, and take semaphores there: esp_coex's init function
 *   (SECONDARY, priority 204) reaches coex_pre_init(), which lives in the
 *   prebuilt libcoexist.a and cannot be modified.
 *
 *   With it on, the board does this on every boot, at t=877 ms:
 *
 *     I (877) coexist: coex firmware version: a830d28
 *     Function[_rt_sem_take] shall not be used before scheduler start
 *     (0) assertion failed at function:_rt_sem_take, line number:574
 *     rst:0x10 (RTCWDT_RTC_RST)          <- boot loop
 *
 *   The S31 port has this option off for the same reason. Keeping ASSERT and
 *   OVERFLOW_CHECK gives the checks that catch real bugs without turning a
 *   legitimate pre-scheduler IDF call into a reset.
 *
 * RT_USING_HOOK is included deliberately: its function-pointer form
 * (RT_HOOK_USING_FUNC_PTR) is what rt_scheduler_stack_overflow_sethook() and
 * the malloc hooks hang off, and it is what an application porting from any
 * other BSP will reach for first.
 * --------------------------------------------------------------------------- */
#define RT_USING_DEBUG
#define RT_DEBUGING_ASSERT
/* RT_DEBUGING_CONTEXT is deliberately not defined - see the header comment. */
#define RT_USING_OVERFLOW_CHECK
#define RT_USING_HOOK
/* Kconfig: if RT_USING_HOOK -> RT_HOOK_USING_FUNC_PTR default y. Without it
 * __ON_HOOK_ARGS() expands to nothing and every RT_OBJECT_HOOK_CALL is a no-op,
 * so the hook setters exist but never fire. */
#define RT_HOOK_USING_FUNC_PTR
#define RT_USING_IDLE_HOOK
#define RT_USING_TIMER_SOFT
#define RT_TIMER_THREAD_PRIO            4
#define RT_TIMER_THREAD_STACK_SIZE      2048
#define RT_USING_MEMPOOL
#define RT_USING_MAILBOX
#define RT_USING_MESSAGEQUEUE
#endif /* RT_CONFIG_H__ */

