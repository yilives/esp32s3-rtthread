# IDF discovers dependencies before Kconfig is available. Do not resolve external
# sources during that pass; the normal pass registers nothing when disabled.
idf_build_get_property(target IDF_TARGET)
idf_build_get_property(non_os_build NON_OS_BUILD)
if(non_os_build)
    idf_component_register()
    return()
endif()
# ESP32-S3 is the only Xtensa target in this port set so far. Resolved before
# any early return so the rest of the file can branch on it once.
if(target STREQUAL "esp32s3")
    set(rtt_xtensa TRUE)
else()
    set(rtt_xtensa FALSE)
endif()

if(CMAKE_BUILD_EARLY_EXPANSION)
    if(rtt_xtensa)
        idf_component_register(PRIV_REQUIRES freertos xtensa)
    elseif(target STREQUAL "esp32c3" OR target STREQUAL "esp32c5" OR
           target STREQUAL "esp32c6" OR target STREQUAL "esp32s31")
        idf_component_register(PRIV_REQUIRES freertos riscv)
    else()
        idf_component_register()
    endif()
    return()
endif()
if(NOT CONFIG_IDF_RTOS_RTTHREAD)
    idf_component_register()
    return()
endif()
if(NOT (target STREQUAL "esp32c3" OR target STREQUAL "esp32c5" OR
        target STREQUAL "esp32c6" OR
        target STREQUAL "esp32s3" OR target STREQUAL "esp32s31"))
    message(FATAL_ERROR "The rtthread component supports only ESP32-C3, ESP32-C5, ESP32-C6, ESP32-S3 and ESP32-S31")
endif()

# Upstream RT-Thread is the only external source tree. Its src/ and include/ are
# used unmodified; everything target specific (rtconfig.h, the CPU port, the
# board init and the stack adapter) lives in this component under port/.
set(root_help "Set RTTHREAD_ROOT to the upstream RT-Thread checkout, using -DRTTHREAD_ROOT=<path> or an environment variable.")
if(DEFINED CACHE{RTTHREAD_ROOT})
    get_property(root_value CACHE RTTHREAD_ROOT PROPERTY VALUE)
else()
    set(root_value "$ENV{RTTHREAD_ROOT}")
endif()
if("${root_value}" STREQUAL "")
    message(FATAL_ERROR "RTTHREAD_ROOT is not set. ${root_help}")
endif()
get_filename_component(root_value "${root_value}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
set(RTTHREAD_ROOT "${root_value}" CACHE PATH "RT-Thread checkout" FORCE)

# RTTHREAD_PORT_ROOT used to point at a forked per-target port checkout that
# duplicated the kernel sources and headers. Those copies are unused: the kernel
# compiles from RTTHREAD_ROOT and the port lives in components/rtthread/port/.
# The variable is still accepted so existing environments and stale CMake cache
# entries keep working; nothing under it is compiled or included any more.
if(DEFINED CACHE{RTTHREAD_PORT_ROOT})
    get_property(port_root_value CACHE RTTHREAD_PORT_ROOT PROPERTY VALUE)
else()
    set(port_root_value "$ENV{RTTHREAD_PORT_ROOT}")
endif()
if(NOT "${port_root_value}" STREQUAL "")
    message(STATUS "rtthread: ignoring deprecated RTTHREAD_PORT_ROOT (${port_root_value}); "
                   "port sources and rtconfig.h now live in components/rtthread/port/.")
endif()

# Kernel and klibc sources compile straight from the upstream checkout; there
# are no local copies. src/ipc.c is needed on both targets: with C3's
# configuration only for the suspend-list helpers, with C6's also for the
# semaphore and mutex objects.
#
# The scheduler comes in two mutually exclusive flavours, and upstream's
# src/SConscript picks between them on RT_USING_SMP:
#
#   cpu_mp.c / scheduler_mp.c       multi-core, per-CPU ready queues, spinlocks
#   cpu_up.c / scheduler_up.c       single core; provides rt_cpu_self(),
#                                   rt_spin_lock_* as no-ops, and the hooks
#
# Both define the same symbols (rt_schedule, rt_cpu_self, rt_spin_lock...), so
# exactly one pair may be compiled.
#
# The switch has to be read out of the target's own rtconfig.h: RT_USING_SMP is
# a C macro there, and CMake has no visibility into it - `if(RT_USING_SMP)`
# would silently always be false and hand an SMP target the UP scheduler.
# Reading the file is the only way to keep the two in step, and it fails loudly
# if the file is missing (checked below as well).
# Where the ESP32-S3 board layer lives.
#
# It is in the RT-Thread tree, at bsp/ESP/ESP32_S3/board, next to the
# architecture layer at libcpu/xtensa. This component compiles the files from
# there instead of keeping a second copy under port/, so what runs on the board
# is what would be contributed upstream. Declared before the config probe below
# because that probe already needs the path.
if(rtt_xtensa)
    set(rtt_s3_bsp_dir "${RTTHREAD_ROOT}/bsp/ESP/ESP32_S3")
    set(rtt_s3_board_dir "${rtt_s3_bsp_dir}/board")
    if(NOT EXISTS "${rtt_s3_board_dir}/board.c")
        message(FATAL_ERROR
            "The ESP32-S3 BSP is missing: ${rtt_s3_board_dir}/board.c not found. "
            "RTTHREAD_ROOT is ${RTTHREAD_ROOT}.")
    endif()
endif()

set(rtt_config_probe "${CMAKE_CURRENT_LIST_DIR}/port/esp32c3/rtconfig.h")
if(target STREQUAL "esp32c5")
    set(rtt_config_probe "${CMAKE_CURRENT_LIST_DIR}/port/esp32c5/rtconfig.h")
elseif(target STREQUAL "esp32c6")
    set(rtt_config_probe "${CMAKE_CURRENT_LIST_DIR}/port/esp32c6/rtconfig.h")
elseif(target STREQUAL "esp32s3")
    set(rtt_config_probe "${rtt_s3_bsp_dir}/rtconfig.h")
elseif(target STREQUAL "esp32s31")
    set(rtt_config_probe "${CMAKE_CURRENT_LIST_DIR}/port/esp32s31/rtconfig.h")
endif()

set(rtt_smp FALSE)
if(EXISTS "${rtt_config_probe}")
    # Anchored so that RT_USING_SMP_SLAVE / RT_USING_SMP_SLAVE_LOCK (the S31
    # second-core options) do not count, and tolerant of a bare `#define
    # RT_USING_SMP` at end of line - measured: requiring trailing whitespace
    # missed the S31 file and silently handed it the UP scheduler, which then
    # failed to compile against the two-argument SMP context-switch prototypes.
    file(STRINGS "${rtt_config_probe}" rtt_smp_lines
         REGEX "^[ \t]*#[ \t]*define[ \t]+RT_USING_SMP([ \t]|$)")
    if(rtt_smp_lines)
        set(rtt_smp TRUE)
    endif()
endif()
message(STATUS "rtthread: RT_USING_SMP=${rtt_smp} (from ${rtt_config_probe})")

if(rtt_smp)
    set(rtt_sched_srcs
        "${RTTHREAD_ROOT}/src/cpu_mp.c"
        "${RTTHREAD_ROOT}/src/scheduler_mp.c")
else()
    set(rtt_sched_srcs
        "${RTTHREAD_ROOT}/src/cpu_up.c"
        "${RTTHREAD_ROOT}/src/scheduler_up.c")
endif()

set(kernel_srcs
    "${RTTHREAD_ROOT}/src/components.c"
    "${RTTHREAD_ROOT}/src/clock.c"
    ${rtt_sched_srcs}
    "${RTTHREAD_ROOT}/src/defunct.c"
    # RT-Thread 5.x moved the device framework out of src/ into the driver tree.
    "${RTTHREAD_ROOT}/components/drivers/core/device.c"
    "${RTTHREAD_ROOT}/src/idle.c"
    "${RTTHREAD_ROOT}/src/ipc.c"
    "${RTTHREAD_ROOT}/src/irq.c"
    "${RTTHREAD_ROOT}/src/kservice.c"
    "${RTTHREAD_ROOT}/src/mem.c"
    # mempool.c defines rt_mp_create()/rt_mp_alloc()/rt_mp_free(). It is a
    # separate translation unit from mem.c and is only compiled by upstream's
    # SConscript when RT_USING_MEMPOOL is on, which is the Kconfig default - so
    # it belongs here for the same reason ipc.c does. Measured: with
    # RT_USING_MEMPOOL enabled but this file absent, the config said the feature
    # was on while every rt_mp_* symbol was missing from the image.
    "${RTTHREAD_ROOT}/src/mempool.c"
    "${RTTHREAD_ROOT}/src/object.c"
    "${RTTHREAD_ROOT}/src/scheduler_comm.c"
    "${RTTHREAD_ROOT}/src/thread.c"
    "${RTTHREAD_ROOT}/src/timer.c"
    "${RTTHREAD_ROOT}/src/klibc/kerrno.c"
    "${RTTHREAD_ROOT}/src/klibc/kstdio.c"
    "${RTTHREAD_ROOT}/src/klibc/kstring.c"
    "${RTTHREAD_ROOT}/src/klibc/rt_vsnprintf_tiny.c"
    "${RTTHREAD_ROOT}/src/klibc/rt_vsscanf.c")

# RT-Thread master split the RISC-V CPU port into libcpu/risc-v/common (the new,
# FDT/DM aware port) and libcpu/risc-v/legacy_common (the flat layout this
# component was written against). The port here needs cpuport.h and
# rt_hw_stack_frame.h at the top of the directory, which is the legacy_common
# layout, so resolve the directory once and use it for both the existence check
# and the include path.
#
# Xtensa's architecture layer lives in the RT-Thread tree, at
# ${RTTHREAD_ROOT}/libcpu/xtensa, in the same place every other architecture
# keeps its port. It is the single copy: this component compiles it from there
# rather than carrying a duplicate under port/, so the code that runs on the
# board is the code that would be contributed upstream.
#
# The board-specific half (console, serial, gpio, pwm, i2c, spi, adc, the
# FreeRTOS-compatibility critical sections) stays in bsp/ESP/ESP32_S3/board, because it
# talks to ESP-IDF's LL headers and has no meaning for another Xtensa vendor.
if(rtt_xtensa)
    set(rtt_riscv_port_dir "")
    set(rtt_xtensa_port_dir "${RTTHREAD_ROOT}/libcpu/xtensa")
    set(rtt_cpu_port_dir "${rtt_xtensa_port_dir}")
    if(NOT EXISTS "${rtt_xtensa_port_dir}/cpuport.h")
        message(FATAL_ERROR
            "The Xtensa port is missing: ${rtt_xtensa_port_dir}/cpuport.h not found. "
            "RTTHREAD_ROOT is ${RTTHREAD_ROOT}.")
    endif()
else()
    set(rtt_riscv_port_dir "${RTTHREAD_ROOT}/libcpu/risc-v/legacy_common")
    if(NOT EXISTS "${rtt_riscv_port_dir}/cpuport.h")
        set(rtt_riscv_port_dir "${RTTHREAD_ROOT}/libcpu/risc-v/common")
    endif()
    set(rtt_cpu_port_dir "${rtt_riscv_port_dir}")
endif()

# Check every compiled kernel source and the public/private header entry points.
# context_gcc.S is retained as the reference for the adapted assembly in port/.
set(rtt_required_files ${kernel_srcs}
    "${RTTHREAD_ROOT}/include/rtthread.h"
    "${RTTHREAD_ROOT}/include/rthw.h")

if(rtt_xtensa)
    # The architecture layer is the upstream-shaped libcpu/xtensa; the BSP half
    # is under bsp/ESP/ESP32_S3/board/.
    list(APPEND rtt_required_files
        "${rtt_xtensa_port_dir}/context_gcc.S"
        "${rtt_xtensa_port_dir}/cpuport.c"
        "${rtt_xtensa_port_dir}/cpuport.h"
        "${rtt_xtensa_port_dir}/backtrace.c"
    "${rtt_xtensa_port_dir}/atomic_xtensa.c"
        "${rtt_s3_bsp_dir}/rtconfig.h"
        "${rtt_s3_board_dir}/board.c"
        "${rtt_s3_board_dir}/console.c")
else()
    list(APPEND rtt_required_files
        "${rtt_riscv_port_dir}/context_gcc.S"
        "${rtt_riscv_port_dir}/cpuport.h"
        "${rtt_riscv_port_dir}/rt_hw_stack_frame.h")
endif()

foreach(required_file IN ITEMS ${rtt_required_files})
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR "RT-Thread source file is missing: ${required_file}. ${root_help}")
    endif()
endforeach()

if(rtt_xtensa)
    # No RISC-V sources and no shared stack adapter: rtthread_stack_init.c
    # assumes the RISC-V rt_hw_stack_frame_t and reads gp/tp, and
    # port/common/context_gcc.S is RISC-V assembly. cpuport.c here defines
    # rt_hw_stack_init() itself.
    set(common_srcs "")
else()
    # Sources shared by both targets.
    # Hardware atomics (amoadd/amoswap). RT_USING_HW_ATOMIC selects these in
    # rtatomic.h; without them every rt_atomic_* call falls back to disabling
    # interrupts, which under SMP means taking the global spinlock - including the
    # rt_atomic_add inside rt_interrupt_enter, i.e. on every trap.
    set(common_srcs
        "port/common/context_gcc.S"
        "rtthread_stack_init.c"
        "${RTTHREAD_ROOT}/libcpu/risc-v/legacy_common/atomic_riscv.c")
endif()

# The shell is part of the upstream checkout and is driven by rtconfig.h.
# shell.c/msh.c/cmd.c/msh_parse.c are compiled unconditionally: every entry
# point they export is guarded by RT_USING_FINSH, and the linker drops the
# whole objects when the shell is off.
set(finsh_srcs
    "${RTTHREAD_ROOT}/components/finsh/shell.c"
    "${RTTHREAD_ROOT}/components/finsh/msh.c"
    "${RTTHREAD_ROOT}/components/finsh/msh_parse.c"
    "${RTTHREAD_ROOT}/components/finsh/cmd.c")

if(rtt_xtensa)
    # RT_USING_DEVICE and RT_USING_SERIAL are on in the BSP rtconfig.h, so
    # the device framework and the serial v1 core are needed. Only the parts
    # this port actually registers:
    #
    #   device.c        the rt_device object model (rt_device_register/find/...)
    #   dev_serial.c    the serial v1 core, including rt_hw_serial_register()
    #   dev_pin.c       rt_device_pin_register() and the rt_pin_* wrappers that
    #                   bsp/ESP/ESP32_S3/board/gpio.c plugs its rt_pin_ops into
    #   rt_drv_pwm.c    rt_device_pwm_register() / rt_pwm_set(), used by
    #                   bsp/ESP/ESP32_S3/board/pwm.c
    #   dev_i2c_core.c  rt_i2c_bus_device_register() / rt_i2c_transfer(), used by
    #                   bsp/ESP/ESP32_S3/board/i2c.c; dev_i2c_dev.c supplies
    #                   rt_i2c_bus_device_device_init(), which the core calls
    #   dev_spi_core.c  rt_spi_bus_register() / rt_spi_transfer() - the whole
    #                   non-DM SPI API, used by bsp/ESP/ESP32_S3/board/spi.c
    #   dev_spi.c       rt_spi_bus_device_init() / rt_spidev_device_init(), which
    #                   the core calls
    #   completion_*    dev_serial.c's TX path waits on an rt_completion
    #
    # dev_spi_bus.c is the device-model (RT_USING_DM) half: it needs
    # struct rt_spi_driver / rt_spi_device_id and rt_device.drv, which the
    # DM-aware rtdevice.h only defines with RT_USING_DM on. Compiling it here
    # fails with "invalid use of undefined type 'struct rt_spi_driver'" and
    # "'struct rt_device' has no member named 'drv'" - measured.
    #
    # The ADC uses the v1 framework (adc.c): v2 needs RT_USING_DM and a device
    # tree, and this port has neither.
    set(serial_srcs
        "${RTTHREAD_ROOT}/components/drivers/core/device.c"
        "${RTTHREAD_ROOT}/components/drivers/serial/dev_serial.c"
        "${RTTHREAD_ROOT}/components/drivers/pin/dev_pin.c"
        "${RTTHREAD_ROOT}/components/drivers/misc/rt_drv_pwm.c"
        "${RTTHREAD_ROOT}/components/drivers/i2c/dev_i2c_core.c"
        "${RTTHREAD_ROOT}/components/drivers/i2c/dev_i2c_dev.c"
        "${RTTHREAD_ROOT}/components/drivers/spi/dev_spi_core.c"
        "${RTTHREAD_ROOT}/components/drivers/spi/dev_spi.c"
        "${RTTHREAD_ROOT}/components/drivers/adc/adc.c"
        "${RTTHREAD_ROOT}/components/drivers/ipc/completion_up.c"
        "${RTTHREAD_ROOT}/components/drivers/ipc/completion_comm.c")
elseif(CONFIG_IDF_TARGET_ESP32C5)
# ESP32-C5 has exactly one device so far: uart0. The list below is therefore the
# serial v1 core plus what its TX path needs, and nothing else.
#
# The S31-shaped list below cannot be reused here, and the reason is not taste:
# it compiles dev_pin.c, dev_i2c_core.c, dev_spi_core.c, adc.c, rt_drv_pwm.c and
# pulse_encoder.c unconditionally, and every one of those files dereferences a
# struct that rtdevice.h only declares when its RT_USING_<class> macro is set.
# Measured with this port's rtconfig.h: pulse_encoder.c fails with
# "invalid use of undefined type 'struct rt_pulse_encoder_device'" and
# 'PULSE_ENCODER_CMD_ENABLE' undeclared - the class is off, the file is not.
#
# Enabling all six classes to make the long list compile would advertise buses
# this port does not drive, so the short list is the honest answer. Add a file
# back here (and its RT_USING_* to rtconfig.h) when its driver exists.
set(serial_srcs
    "${RTTHREAD_ROOT}/components/drivers/serial/dev_serial.c"
    # Pin device: rt_device_pin_register() and the rt_pin_* wrappers that
    # port/esp32c5/gpio.c plugs its rt_pin_ops into. It also carries the msh
    # `pin` command, so it cannot be dropped without the driver becoming
    # unreachable from the shell.
    "${RTTHREAD_ROOT}/components/drivers/pin/dev_pin.c"
    # PWM device class: rt_device_pwm_register()/rt_pwm_set()/rt_pwm_enable(),
    # which port/esp32c5/pwm.c plugs its rt_pwm_ops into. Only compiled now that
    # RT_USING_PWM is on in the C5 rtconfig.h - without that macro rtdevice.h
    # does not declare struct rt_device_pwm and this file does not compile.
    "${RTTHREAD_ROOT}/components/drivers/misc/rt_drv_pwm.c"
    # I2C device class: rt_i2c_bus_device_register() and rt_i2c_transfer(),
    # which port/esp32c5/i2c.c plugs its rt_i2c_bus_device_ops into. dev_i2c_dev.c
    # supplies rt_i2c_bus_device_device_init() that core calls - without it the
    # link fails with exactly that one undefined symbol (measured on S3/S31).
    "${RTTHREAD_ROOT}/components/drivers/i2c/dev_i2c_core.c"
    "${RTTHREAD_ROOT}/components/drivers/i2c/dev_i2c_dev.c"
    # ADC device class (v1 framework): rt_hw_adc_register(), rt_adc_enable(),
    # rt_adc_read(), which port/esp32c5/adc.c plugs its rt_adc_ops into. This
    # one is not optional - without it the link fails with undefined references
    # to exactly those three symbols (measured).
    "${RTTHREAD_ROOT}/components/drivers/adc/adc.c"
    # SPI bus class: rt_spi_bus_register() and rt_spi_transfer(), which
    # port/esp32c5/spi.c plugs its rt_spi_ops into. dev_spi.c supplies
    # rt_spidev_device_init() that the core calls.
    "${RTTHREAD_ROOT}/components/drivers/spi/dev_spi_core.c"
    "${RTTHREAD_ROOT}/components/drivers/spi/dev_spi.c"
    # Pulse encoder class: rt_device_pulse_encoder_register() and the
    # rt_device_read() path that returns the count, which port/esp32c5/pcnt.c
    # plugs its rt_pulse_encoder_ops into. Only compiles now that
    # RT_USING_PULSE_ENCODER is on - without that macro rtdevice.h does not
    # declare struct rt_pulse_encoder_device.
    "${RTTHREAD_ROOT}/components/drivers/misc/pulse_encoder.c"
    # CAN device class: rt_can_ops, the software RX FIFO, rt_hw_can_isr.
    "${RTTHREAD_ROOT}/components/drivers/can/dev_can.c"
    # dev_can.c's non-blocking TX path and its ISR use rt_ringbuffer_*, which
    # lives in ipc/ringbuffer.c. Nothing else in this port pulled that member in,
    # so the class compiled and the link failed on five ringbuffer symbols.
    "${RTTHREAD_ROOT}/components/drivers/ipc/ringbuffer.c"
    # completion_* moved to components/drivers/ipc/ in 5.x; dev_serial.c's TX path
    # waits on an rt_completion. Single core -> completion_up.c.
    "${RTTHREAD_ROOT}/components/drivers/ipc/completion_up.c"
    "${RTTHREAD_ROOT}/components/drivers/ipc/completion_comm.c")

else()
# Serial framework, v1. dev_serial.c is the only file needed: the tty half is
# behind RT_USING_SMART, and bypass/DMA are optional.
# completion_* moved to components/drivers/ipc/ in 5.x; dev_serial.c needs
# rt_completion_wait/init/done for its TX path. Single core -> completion_up.c.
# dev_pin.c provides rt_device_pin_register() and the rt_pin_* wrappers that
# port/esp32s31/gpio.c plugs its rt_pin_ops into.
set(serial_srcs
    "${RTTHREAD_ROOT}/components/drivers/serial/dev_serial.c"
    "${RTTHREAD_ROOT}/components/drivers/pin/dev_pin.c"
    "${RTTHREAD_ROOT}/components/drivers/misc/rt_drv_pwm.c"
    "${RTTHREAD_ROOT}/components/drivers/misc/pulse_encoder.c"
    # dev_i2c_bus.c is the device-model (RT_USING_DM) half: it needs
    # struct rt_i2c_driver / rt_i2c_client with a `parent` rt_device, which the
    # DM-aware rtdevice.h only defines with RT_USING_DM on. dev_i2c_core.c is
    # the bus registration and rt_i2c_transfer() implementation, and
    # dev_i2c_dev.c supplies rt_i2c_bus_device_device_init(), which core calls -
    # without it the link fails with exactly that one undefined symbol.
    "${RTTHREAD_ROOT}/components/drivers/i2c/dev_i2c_core.c"
    "${RTTHREAD_ROOT}/components/drivers/spi/dev_spi_core.c"
    "${RTTHREAD_ROOT}/components/drivers/spi/dev_spi.c"
    "${RTTHREAD_ROOT}/components/drivers/adc/adc.c"
    "${RTTHREAD_ROOT}/components/drivers/i2c/dev_i2c_dev.c"
    "${RTTHREAD_ROOT}/components/drivers/ipc/completion_up.c"
    "${RTTHREAD_ROOT}/components/drivers/ipc/completion_comm.c")
endif() # rtt_xtensa

if(CONFIG_IDF_TARGET_ESP32C3)
    set(target_srcs
        "port/esp32c3/cpuport.c"
        "port/esp32c3/interrupt.c")
    # rtconfig.h is the only header here; the adjacent port C sources cannot
    # shadow upstream kernel headers.
    set(rtt_config_dir "${CMAKE_CURRENT_LIST_DIR}/port/esp32c3")
elseif(CONFIG_IDF_TARGET_ESP32C6)
    set(target_srcs
        "port/esp32c6/cpuport.c")
    set(rtt_config_dir "${CMAKE_CURRENT_LIST_DIR}/port/esp32c6")
elseif(CONFIG_IDF_TARGET_ESP32S3)
     # Architecture: from libcpu/xtensa, the single copy.
     #
     #   context_gcc.S  the five XT_RTOS_* callouts, rt_hw_context_switch*,
     #                  rt_hw_interrupt_disable/enable - and nothing else, because
     #                  components/xtensa/xtensa_vectors.S already owns the vector
     #                  table, the window overflow/underflow handlers, the
     #                  level 2..6 and debug vectors and _xt_panic
     #   cpuport.c      rt_hw_stack_init(), the ISR stack, the globals the
     #                  assembly needs, and the FPU policy
     #   backtrace.c    rt_hw_backtrace_frame_get/_unwind over the base-save area
     #
     # Board: from bsp/ESP/ESP32_S3/board, because these talk to ESP-IDF.
     #
     #   interrupt.c    rt_hw_interrupt_install/mask/umask onto
     #                  xt_set_interrupt_handler()
     #   critical.c     the FreeRTOS port API IDF components call directly
     #   board.c        rt_hw_board_init(): SYSTIMER tick, coprocessor init, FPU
     #                  enable, and the two spi_flash hooks a unicore build needs
     #   console.c      rt_hw_console_output/getchar over the ROM and the UART LL
     #
     # board.c is added by ${board_src} below, not here, so it is not compiled
     # twice.
    set(target_srcs
        "${rtt_xtensa_port_dir}/context_gcc.S"
        "${rtt_xtensa_port_dir}/cpuport.c"
        "${rtt_xtensa_port_dir}/backtrace.c"
        "${rtt_xtensa_port_dir}/atomic_xtensa.c"
        "${rtt_s3_board_dir}/critical.c"
        "${rtt_s3_board_dir}/interrupt.c"
        "${rtt_s3_board_dir}/console.c"
        "${rtt_s3_board_dir}/serial.c"
        "${rtt_s3_board_dir}/gpio.c"
        "${rtt_s3_board_dir}/pwm.c"
        "${rtt_s3_board_dir}/i2c.c"
        "${rtt_s3_board_dir}/spi.c"
        "${rtt_s3_board_dir}/adc.c"
        "${rtt_s3_board_dir}/smp.c")
    set(rtt_config_dir "${rtt_s3_bsp_dir}")

    # FreeRTOS-compatible primitives on rt_*. Target independent - the same files
    # the S31 branch uses - and required as soon as an IDF component that speaks
    # FreeRTOS APIs is linked in: esp_event, esp_netif, lwip, esp_wifi and the
    # NimBLE host all create queues, semaphores and tasks.
    #
    # ft_ipc/ft_qset/ft_event/ft_task are listed unconditionally: they are the
    # machinery ft_core.h's rt_*-backed objects are built on, they cost nothing
    # when unused (--gc-sections drops them), and listing them now means enabling
    # Wi-Fi later does not also require remembering to add them.
    #
    # The thin xQueueCreate -> ft_ipc_create layer IS conditional: defining those
    # symbols while libfreertos.a still defines them makes the winner depend on
    # archive order. That is exactly what CONFIG_IDF_RTOS_RTTHREAD_FREERTOS_SHIM
    # gates, and the rule is the same here as on the S31 branch.
    list(APPEND target_srcs
        "port/freertos/ft_ipc.c"
        "port/freertos/ft_qset.c"
        "port/freertos/ft_event.c"
        "port/freertos/ft_task.c")

    if(CONFIG_IDF_RTOS_RTTHREAD_FREERTOS_SHIM)
        list(APPEND target_srcs
            "port/freertos/freertos_api.c"
            "port/freertos/ft_query.c"
            "port/freertos/ft_probe.c"
            "port/freertos/ft_notify.c")
    endif()
elseif(CONFIG_IDF_TARGET_ESP32S31)
    set(target_srcs
        "port/esp32s31/cpuport.c"
        "port/esp32s31/interrupt.c"
        "port/esp32s31/serial.c"
        "port/esp32s31/gpio.c"
        "port/esp32s31/i2c.c"
        "port/esp32s31/boot_tasks.c"
        "port/esp32s31/pwm.c"
        "port/esp32s31/gptimer.c"
        "port/esp32s31/selftest.c"
        "port/esp32s31/board_extra.c"
        "port/esp32s31/intr_debug.c"
        "port/esp32s31/spi.c"
        "port/esp32s31/adc.c"
        "port/esp32s31/pcnt.c"
        "port/esp32s31/mcpwm.c"
        "port/esp32s31/rmt.c"
        "port/esp32s31/twai.c"
        "port/esp32s31/twai_cmd.c"
        "port/esp32s31/smp_prims.c"
        "port/esp32s31/smp_ipi.c"
        "port/esp32s31/smp_stack.c"
        "port/esp32s31/smp_slave.c"
        "port/esp32s31/smp_slave_sched.c"
        "port/esp32s31/smp_tick.c"
        "port/esp32s31/smp_istack.c"
        "port/esp32s31/smp_cmd.c"
        "port/esp32s31/console_probe.c"
        "port/esp32s31/usb_probe.c"
        "port/esp32s31/lp_core.c"
        "port/esp32s31/sleep.c"
        )
    # FreeRTOS-compatible primitives on rt_*. Target independent, but only S31
    # needs them today; they exist so the IDF components that call FreeRTOS APIs
    # (esp_driver_uart, esp_event, esp_netif, lwip, esp_wifi) can run on
    # RT-Thread without being rewritten. See port/freertos/ft_core.h.
    list(APPEND target_srcs
        "port/freertos/ft_ipc.c"
        "port/freertos/ft_qset.c"
        "port/freertos/ft_event.c"
        "port/freertos/ft_task.c")

    # The thin xQueueCreate -> ft_ipc_create mapping. Only built when the real
    # FreeRTOS scheduler is switched off: defining those symbols while
    # libfreertos.a still defines them makes the winner depend on archive order.
    if(CONFIG_IDF_RTOS_RTTHREAD_FREERTOS_SHIM)
        list(APPEND target_srcs
            "port/freertos/freertos_api.c"
            "port/freertos/ft_query.c"
            "port/freertos/ft_probe.c"
            "port/freertos/ft_notify.c")
    endif()
    set(rtt_config_dir "${CMAKE_CURRENT_LIST_DIR}/port/esp32s31")
elseif(CONFIG_IDF_TARGET_ESP32C5)
    # The C5 is the C6's CLIC sibling: one RV32IMAC core, a CLIC with hardware
    # nesting, no FPU. It reuses the C6 source set - the CPU port, the interrupt
    # bridge and the UART0 console driver - because port/esp32c6 and
    # port/esp32c5 differ only in the SoC name and in the two trap-path tables
    # cpuport.c has to define for a single-core build.
    #
    # gpio.c, pwm.c, i2c.c, spi.c and adc.c are deliberately absent: they are
    # written against the S31's LL headers and pin maps and have not been brought
    # up here. `list device` therefore shows uart0 and pin only, which is the
    # honest state of this port.
    set(target_srcs
        "port/esp32c5/cpuport.c"
        "port/esp32c5/interrupt.c"
        "port/esp32c5/serial.c"
        "port/esp32c5/gpio.c"
        # LEDC as an rt_device_pwm. Needs RT_USING_PWM and rt_drv_pwm.c, both of
        # which are already in this target's serial_srcs list above.
        "port/esp32c5/pwm.c"
        # I2C master on the I2C LL. Needs RT_USING_I2C and dev_i2c_core.c, which
        # are added to serial_srcs below.
        "port/esp32c5/i2c.c"
        # SAR ADC oneshot as an rt_adc_device. Needs RT_USING_ADC and the adc.c
        # framework file, which are added to serial_srcs below.
        "port/esp32c5/adc.c"
        # GPSPI master (SPI2) as an rt_spi_bus. Needs RT_USING_SPI and
        # dev_spi_core.c, which are added to serial_srcs below.
        "port/esp32c5/spi.c"
        # TWAI (CAN / CAN-FD) as a plain C API - the hardware path first, an
        # rt_can device class later. Needs no RT-Thread class macro.
        "port/esp32c5/twai.c"
        # RMT TX + RX as a plain C API. RT-Thread has no RMT class.
        "port/esp32c5/rmt.c"
        # PCNT as an rt_pulse_encoder device (`pcnt0`). Needs
        # RT_USING_PULSE_ENCODER and pulse_encoder.c.
        "port/esp32c5/pcnt.c"
        # MCPWM as a plain C API. RT-Thread has no motor-control class.
        "port/esp32c5/mcpwm.c"
        # Temperature sensor as a plain C API. It lives in the SAR ADC block.
        "port/esp32c5/tsens.c"
        # PSRAM: state reporting and a read/write check. Needs CONFIG_SPIRAM.
        # NOTE: the LP core (ULP RISC-V) driver is NOT here. An LP program is a
        # property of the project, not of the port: ulp_add_project() attaches the
        # generated shared-symbol linker script to the component that calls it, so
        # the shared variables are only visible from that component. Ours lives in
        # main/ulp_selftest.c for that reason.
        "port/esp32c5/psram.c"
        # Pin ownership table and the conflict report. Not a driver.
        "port/esp32c5/resources.c"
        # USB Serial/JTAG: the in-chip USB device controller. Pads IO13/IO14.
        "port/esp32c5/usj.c"
        # AHB GDMA: one TX channel, memory-to-memory. PARLIO needs it.
        "port/esp32c5/gdma.c"
        # PARLIO TX: parallel output fed by GDMA, verified against PCNT.
        "port/esp32c5/parlio.c"
        # TWAI as an rt_can device (`can0`).
        "port/esp32c5/can.c"
        # Deep sleep: what survives the HP core being powered down.
        "port/esp32c5/lp_sleep.c"
        # LP I2C: the low-power domain I2C controller as `lp_i2c0`.
        "port/esp32c5/lp_i2c.c")

    # The FreeRTOS-API compatibility layer, target independent - the same files
    # the S31 uses. Without this block a C5 build with
    # CONFIG_IDF_RTOS_RTTHREAD_FREERTOS_SHIM=y does NOT link: measured, the first
    # attempt produced undefined references to xQueueGenericCreate,
    # xQueueSemaphoreTake, xTaskGetSchedulerState, vTaskSuspendAll, vTaskDelay and
    # more, from spi_flash, esp_libc/src/locks.c and freertos/portmacro.h. The
    # Kconfig switch turns the real FreeRTOS scheduler off; nothing turned the
    # shim on, because the shim was only listed for S3 and S31.
    #
    # ft_ipc/ft_qset/ft_event/ft_task are the machinery the whole shim is built
    # on and cost nothing when unused; the thin xQueueCreate -> ft_ipc_create
    # layer is conditional, because defining those symbols while libfreertos.a
    # still defines them makes the winner depend on archive order.
    list(APPEND target_srcs
        "port/freertos/ft_ipc.c"
        "port/freertos/ft_qset.c"
        "port/freertos/ft_event.c"
        "port/freertos/ft_task.c")

    if(CONFIG_IDF_RTOS_RTTHREAD_FREERTOS_SHIM)
        list(APPEND target_srcs
            "port/freertos/freertos_api.c"
            "port/freertos/ft_query.c"
            "port/freertos/ft_probe.c"
            "port/freertos/ft_notify.c")
    endif()

    set(rtt_config_dir "${CMAKE_CURRENT_LIST_DIR}/port/esp32c5")
endif()

# These flags are applied identically on both targets: the kernel, the shared
# stack adapter, the context assembly and the CPU port. port/common/board.c
# keeps the component defaults because it calls into IDF.
set_source_files_properties(${kernel_srcs} ${common_srcs} ${target_srcs} ${serial_srcs} PROPERTIES
    COMPILE_DEFINITIONS "__RTTHREAD__;__RT_KERNEL_SOURCE__"
    COMPILE_OPTIONS "-ffreestanding;-fno-builtin")

# rtthread.h pulls in finsh.h whenever RT_USING_FINSH is set, and finsh.h stores
# commands as `long (*)(void)` and casts to the real prototype at call time.
# Any driver that exports an msh command therefore trips
# -Werror=cast-function-type: that is the upstream design, not a local bug.
set_source_files_properties(${finsh_srcs} ${serial_srcs} PROPERTIES
    COMPILE_DEFINITIONS "__RTTHREAD__;__RT_KERNEL_SOURCE__"
    COMPILE_OPTIONS "-ffreestanding;-fno-builtin;-Wno-cast-function-type")

if(rtt_xtensa)
    # The Xtensa port supplies its own board init (bsp/ESP/ESP32_S3/board/board.c): the
    # shared one is RISC-V all the way down (riscv/interrupt.h,
    # esprv_int_set_threshold, the CLIC sweep, the device-model drivers).
    set(board_src "${rtt_s3_board_dir}/board.c")
else()
    set(board_src "port/common/board.c")
endif()

set(srcs ${kernel_srcs} ${common_srcs} ${target_srcs} ${finsh_srcs} ${serial_srcs} ${board_src})

if(CONFIG_IDF_TARGET_ESP32C5)
    # dev_can.c relies on a `/* FALLTHROUGH: ... */` comment to mark a
    # deliberate case fall-through in rt_hw_can_isr. GCC only accepts the
    # bare `/* fall through */` spellings at -Wimplicit-fallthrough=3, which
    # this build enables, so upstream's marker is not recognised and the file
    # fails on a warning. Suppressing it for this one file is narrower than
    # changing the shared build flags.
    set_source_files_properties("${RTTHREAD_ROOT}/components/drivers/can/dev_can.c" PROPERTIES
        COMPILE_OPTIONS "-Wno-implicit-fallthrough;-Wno-cast-function-type")
    # i2c.c exports an `i2cscan` msh command through MSH_CMD_EXPORT_ALIAS, which
    # hits the same cast-function-type warning as finsh itself - the upstream
    # design, not a local bug. Same treatment as the S31 block below.
    set_source_files_properties("port/esp32c5/i2c.c" PROPERTIES
        COMPILE_DEFINITIONS "__RTTHREAD__;__RT_KERNEL_SOURCE__"
        COMPILE_OPTIONS "-ffreestanding;-fno-builtin;-Wno-cast-function-type")
endif()

if(CONFIG_IDF_TARGET_ESP32S31)
    # The self test and the timer probe export msh commands through
    # MSH_CMD_EXPORT_ALIAS, which hits the same cast-function-type warning as
    # finsh itself.
    set_source_files_properties("port/esp32s31/selftest.c" "port/esp32s31/board_extra.c"
                            "port/esp32s31/intr_debug.c" "port/esp32s31/gpio.c"
                            "port/esp32s31/spi.c"
                            "port/esp32s31/adc.c"
                            "port/esp32s31/pcnt.c"
                            "port/esp32s31/mcpwm.c"
                            "port/esp32s31/rmt.c"
                            "port/esp32s31/twai.c"
                            "port/esp32s31/twai_cmd.c"
                            "port/esp32s31/smp_prims.c"
                            "port/esp32s31/smp_ipi.c"
                            "port/esp32s31/smp_stack.c"
                            "port/esp32s31/smp_slave.c"
                            "port/esp32s31/smp_slave_sched.c"
                            "port/esp32s31/smp_tick.c"
                            "port/esp32s31/smp_istack.c"
                            "port/esp32s31/smp_cmd.c"
                            "port/esp32s31/console_probe.c"
                            "port/esp32s31/usb_probe.c"
                            "port/esp32s31/lp_core.c"
                            "port/esp32s31/sleep.c"

                            "port/esp32s31/gptimer.c"
                            "port/esp32s31/pwm.c"
                            "port/esp32s31/i2c.c" PROPERTIES
        COMPILE_DEFINITIONS "__RTTHREAD__;__RT_KERNEL_SOURCE__"
        COMPILE_OPTIONS "-ffreestanding;-fno-builtin;-Wno-cast-function-type")
endif()

if(rtt_xtensa)
    idf_component_register(SRCS ${srcs}
        # Upstream headers first, then this component's per-target config directory
        # (rtconfig.h).
        #
        # cpuport.h now comes from libcpu/xtensa (PRIV_INCLUDE_DIRS below), not
        # from the config directory: it is the architecture layer's header and
        # carries the frame constants plus its own xtensa_context.h include.
        INCLUDE_DIRS "${RTTHREAD_ROOT}/include" "${RTTHREAD_ROOT}/components/finsh"
                     "${RTTHREAD_ROOT}/components/drivers/include" "${rtt_config_dir}"
        PRIV_INCLUDE_DIRS "${rtt_xtensa_port_dir}" "${rtt_s3_bsp_dir}" "${IDF_PATH}/components/freertos"
                          "${IDF_PATH}/components/freertos/FreeRTOS-Kernel/portable/xtensa/include/freertos"
                          "${CMAKE_CURRENT_LIST_DIR}/port/freertos"
        PRIV_REQUIRES freertos xtensa esp_rom soc hal esp_hal_uart esp_hal_systimer heap
                      esp_hal_gpio esp_hal_ledc
        LDFRAGMENTS "linker.lf")
else()
idf_component_register(SRCS ${srcs}
    # Upstream headers first, then this component's per-target config directory
    # (rtconfig.h) and cpuport.h / rt_hw_stack_frame.h from upstream libcpu.
    INCLUDE_DIRS "${RTTHREAD_ROOT}/include" "${RTTHREAD_ROOT}/components/finsh"
                 "${RTTHREAD_ROOT}/components/drivers/include" "${rtt_config_dir}"
    # The last three are ulp's, named directly.
    #
    # PRIV_REQUIRES ulp alone was not enough: the dependency was recorded (ulp
    # appears in build_components, and the include count on lp_core.c's compile
    # line went 42 -> 59) but ulp/lp_core/include was still absent, so
    # lp_core.c could not find ulp_lp_core.h. The reason is that ulp's own
    # INCLUDE_DIRS are conditional on CONFIG_ULP_COPROC_ENABLED at the moment it
    # registers, and this component registers before that resolves. Naming the
    # directories is explicit and does not depend on that ordering.
    PRIV_INCLUDE_DIRS "${rtt_riscv_port_dir}" "${IDF_PATH}/components/freertos"
                      "${CMAKE_CURRENT_LIST_DIR}/port/freertos"
                      "${IDF_PATH}/components/ulp/lp_core/include"
                      "${IDF_PATH}/components/ulp/lp_core/shared/include"
                      "${IDF_PATH}/components/ulp/ulp_common/include"
    # esp_hw_support: port/esp32c5/gpio.c calls esp_intr_alloc(), which lives in
    # esp_hw_support/intr_alloc.c. It is not part of the `hal` umbrella, so it has
    # to be named; without it the gpio object fails with "esp_intr_alloc.h: No
    # such file or directory".
    PRIV_REQUIRES freertos riscv esp_rom soc hal esp_hal_uart esp_hal_gpio
                  esp_hw_support esp_hal_i2c esp_hal_systimer heap ulp
    LDFRAGMENTS "linker.lf")
endif()

# IDF applies these properties via generator expressions, including to components
# registered earlier (startup, the FreeRTOS tick/port adapters, and applications).
# rtconfig.h must stay globally visible because those include <rtthread.h>.
# cpuport.h likewise: under RT_USING_SMP, rttypes.h includes it for
# rt_hw_spinlock_t, from every translation unit that sees <rtthread.h>.
idf_build_set_property(INCLUDE_DIRECTORIES
    "${RTTHREAD_ROOT}/include;${RTTHREAD_ROOT}/components/finsh;${RTTHREAD_ROOT}/components/drivers/include;${rtt_config_dir};${rtt_cpu_port_dir}" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS RT_USING_USER_MAIN APPEND)
# RT-Thread invokes the application's main from a kernel archive. Keep that
# entry point regardless of the component archive order (as IDF does for app_main).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u main")
if(NOT rtt_xtensa)
    # The trap entry lives in the riscv component's assembly, which the linker reads
    # after this archive. Without a reference planted ahead of it, the archive member
    # holding rt_hw_irq_nest_addr is never pulled in and the link fails with
    # "undefined reference" from vectors_rtthread.S.
    #
    # Nothing equivalent is needed on Xtensa: _xt_lowint1 and the entry table are
    # in libxtensa.a, which components/xtensa/xtensa_vectors.S is part of, and
    # librtthread.a references _xt_context_save from its own context_gcc.S - so
    # the archive member is pulled in by an ordinary undefined reference.
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u rt_irq_nest_slot" "-u rt_irq_stack_top")
endif()

# finsh only exports section-based registration data; no symbol in that object
# is referenced, so the linker would drop the archive members and the .rti_fn
# table would hold nothing but its boundary markers. Force the entry point in.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u finsh_system_init")
# Same problem, one level further out: finsh/cmd.c defines `list`, `clear`,
# `version` and `console` purely through MSH_CMD_EXPORT_ALIAS into the KEEP()'d
# FSymTab section. Nothing references any of its global symbols, so
# --gc-sections drops the whole member and the shell silently loses `list` -
# the command that is supposed to show threads, timers, semaphores, devices and
# memory. `help` still works, which is what makes this hard to notice: it just
# lists fewer commands.
#
# The symbol has to be a GLOBAL one: `cmd_list` itself is static, so `-u cmd_list`
# would be silently ignored by the linker and the member would stay dropped - a
# trap worth spelling out because it looks like the obvious choice.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u list_thread")
if(CONFIG_IDF_TARGET_ESP32S31)
    # Same archive-ordering problem: the application calls rt_hw_uart0_init()
    # but libmain.a is scanned after librtthread.a, so the member would never be
    # pulled in. -u plants the undefined reference before the archive is read.
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u rt_hw_uart0_init")
endif()

if(CONFIG_IDF_TARGET_ESP32C5)
    # port/esp32c5/serial.c registers uart0 through INIT_BOARD_EXPORT and also
    # carries rt_hw_console_output(), which rt_kprintf resolves through the weak
    # stub in kservice.c. Both are data-section entries or weak, so neither
    # creates the reference that would pull the member out of librtthread.a -
    # --gc-sections drops it and the board then boots with no console at all.
    # Measured on S31 with the same arrangement.
    #
    # rt_hw_pin_init is the same story one file further on: measured here, the
    # first build with port/esp32c5/gpio.c compiled the object (it is present in
    # build/esp-idf/rtthread/CMakeFiles as gpio.c.obj) and the linked image
    # contained neither rt_hw_pin_init nor the `pin` command, so `list device`
    # showed uart0 only.
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u rt_hw_uart0_init" "-u rt_hw_pin_init")
    # Same reason again for pwm.c: its rt_hw_pwm_init() is referenced only by an
    # INIT_BOARD_EXPORT data entry, so --gc-sections would drop the member and
    # `list device` would show no pwm0 with no error anywhere.
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u rt_hw_pwm_init")
    # And a third time for i2c.c. This one anchor also brings in the `i2cscan`
    # command, because both live in the same archive member - `-u cmd_i2cscan`
    # would NOT work, since that handler is static and `-u` creates an external
    # reference a local symbol cannot satisfy (see the note in the xtensa block).
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u rt_hw_i2c_init")
    # adc.c and spi.c, same reason each time. Measured: without the spi one the
    # object compiled, spi.c.obj is in the build tree, and the linked image
    # contains no rt_hw_spi_init at all - `list device` just never shows spi0.
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u rt_hw_adc_init")
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u rt_hw_spi_init")
    # can.c registers can0 from INIT_DEVICE_EXPORT, a data-section entry, so
    # --gc-sections drops the member and `list device` shows no can0 - the
    # same shape as the seven above. This one anchor also brings in `c5can`,
    # which lives in the same archive member.
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u rt_hw_can_c5_init")
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u rt_hw_pin_init" "-u c5_probe_pads")
    # lp_sleep.c has no init function of its own - it is driven from the
    # shell and from the boot banner - so the anchor is its command.
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u c5_lp_sleep_selftest")
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u c5_lp_i2c_selftest")
endif()

if(rtt_xtensa)
    # Two archive members on ESP32-S3 that nothing references by symbol, and
    # that --gc-sections therefore drops - together with the code that makes
    # them matter:
    #
    #   rt_hw_uart0_init   INIT_BOARD_EXPORT() puts it in the .rti_fn table, and
    #                      a table entry is data, not a reference. Without this,
    #                      uart0 is never registered, rt_console_set_device()
    #                      has nothing to find, and `list device` is empty -
    #                      measured exactly that way: the serial device was
    #                      compiled and simply absent from the image.
    #
    # dev_serial's read/write come in through the rt_uart_ops table once the
    # device exists, so one -u is enough for the whole driver.
    #
    #   rt_hw_pin_init     same mechanism for bsp/ESP/ESP32_S3/board/gpio.c: its
    #                      INIT_BOARD_EXPORT() entry is a table entry, so
    #                      --gc-sections drops the member and the whole file
    #                      with it - measured: after a clean build the symbol
    #                      was simply absent from the ELF and `list device`
    #                      showed uart0 only.
    #   rt_hw_pwm_init     ditto for bsp/ESP/ESP32_S3/board/pwm.c.
    #   rt_hw_i2c_init     ditto for bsp/ESP/ESP32_S3/board/i2c.c.
    #   cmd_i2c            i2cscan is exported only through FSymTab - a data
    #                      section - so nothing references it and the command
    #                      silently disappears from the shell.
    #   rt_hw_spi_init     ditto for bsp/ESP/ESP32_S3/board/spi.c.
    #   cmd_spiloop        ditto for the SPI loopback command.
    #   rt_hw_adc_init     ditto for bsp/ESP/ESP32_S3/board/adc.c.
    #
    # Note on the command symbols: `-u cmd_i2c` and friends do NOT work and are
    # not used. Those handlers are `static`, and `-u` creates an *external*
    # undefined reference that a local symbol cannot satisfy, so ld leaves it
    # dangling (without an error, because -u means "force", not "require"). What
    # actually pulls those objects in is their init function above, which carries
    # the command along with it - verified in the ELF: cmd_i2c etc. appear as
    # local text next to the `-u` leftovers.
    #
    # For libcpu/xtensa/backtrace.c the anchor is its shell command. The two
    # rt_hw_backtrace_frame_* overrides cannot be used as anchors: kservice.c
    # defines weak versions of both in the SAME archive, so `-u` is satisfied by
    # those and this member is never pulled in - measured, and the board then
    # printed "rt_hw_backtrace_frame_unwind is not implemented" with the object
    # sitting unused in librtthread.a.
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u rt_hw_uart0_init" "-u rt_hw_pin_init"
                                                     "-u rt_hw_pwm_init" "-u rt_hw_i2c_init"
                                                     "-u rt_hw_spi_init" "-u rt_hw_adc_init"
                                                     "-u xt_rt_backtrace_cmd")
endif()

# rt_hw_us_delay is declared in rthw.h and weak-stubbed in kservice.c. Our strong
# definition lives in an archive member nothing calls, so without this the weak stub
# wins and every POSIX sleep() silently becomes a no-op. Verified with nm: the symbol
# was absent from the image before this line.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u rt_hw_us_delay")

if(CONFIG_IDF_TARGET_ESP32S31)
# Same reason: smp_tick.c has no referrer, so its INIT_APP_EXPORT entry would be
# dropped along with the object and the CPU1 report would silently vanish.
# Measured: the symbol was in smp_tick.c.obj and absent from the ELF.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u s31_cpu1_report")

# usb_probe.c is a single MSH command and nothing calls into it, so --gc-sections drops
# the object and `s31usb` reports "command not found" even though the source compiled.
# Measured: adding the file to target_srcs alone was not enough; the command appeared only
# after this line was added.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u cmd_usb")

# Deep sleep, for the experiment of whether it links as-is.
#
# sleep_modes.c compiles today and all 43 of its undefined symbols exist as object
# files in the build tree, but nothing in the RT-Thread image references any of
# them, so --gc-sections drops the whole graph. Anchoring one entry point pulls in
# whatever it actually needs, which turns "would deep sleep link?" from a guess
# into a link error or the absence of one.
if(CONFIG_RTT_SLEEP_LINK_PROBE)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u esp_deep_sleep_start")
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u esp_sleep_enable_timer_wakeup")
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u esp_sleep_get_wakeup_cause")

    # The LP core launcher, same question: does it link with no glue?
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u ulp_lp_core_run")
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u ulp_lp_core_load_binary")
endif()

# The two new shell commands are single MSH entries and nothing references them.
# Same reason as cmd_usb above: without these the objects are dropped and the
# commands report "not found" even though their sources compiled.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u cmd_lp")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u cmd_sleep")

# sleep.c's INIT_APP_EXPORT writes a separate record, __rt_init_s31_sleep_report.
# Anchoring cmd_sleep pulls the object in but does not force that record into the
# table: measured, cmd_sleep was present in the ELF while the init entry was not,
# so `s31sleep` would report a previous cycle but the boot-time report would never
# run. One -u per record.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u __rt_init_s31_sleep_report")

# INIT_BOARD_EXPORT / INIT_DEVICE_EXPORT need the archive member to be pulled in,
# and on this build it is not: measured, `rt_hw_pin_init` was absent from the image
# after it was exported and its hand call in board.c was removed, so `list device`
# lost `pin`. Upstream BSPs compile every source into one image, so a section
# reference is enough there; with ESP-IDF each component is an archive and
# --gc-sections drops members nothing references. One `-u` per exported init keeps
# the standard RT-Thread pattern while making the archive model explicit.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-u rt_hw_pin_init"
    "-u rt_hw_pwm_init"
    "-u rt_hw_timer_init"
    "-u rt_hw_i2c_init"
    "-u rt_hw_spi_init"
    "-u rt_hw_adc_init"
    "-u rt_hw_pcnt_init"
    "-u rt_hw_mcpwm_init"
    "-u rt_hw_tsens_init"
    "-u rt_hw_rmt_init"
    "-u rt_hw_twai_init")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    RT_MAIN_THREAD_STACK_SIZE=CONFIG_ESP_MAIN_TASK_STACK_SIZE)

# esp_hal_ledc, esp_hal_timg and esp_hal_i2c are the HAL components
# port/esp32s31/{pwm,gptimer,i2c}.c reach directly. They are not part of the
# `hal` umbrella - IDF only pulls them in through esp_driver_ledc /
# esp_driver_gptimer / esp_driver_i2c, which this port does not use - and
# listing them in PRIV_REQUIRES does not put their include directories on this
# component's compile line (the -I list in build/compile_commands.json stays
# unchanged). Linking the component aliases does carry both the headers and the
# archives, which is what the LEDC signal table in ledc_periph.c needs at link
# time.
if(CONFIG_IDF_TARGET_ESP32S31)
target_link_libraries(${COMPONENT_LIB} PRIVATE idf::esp_hal_ledc idf::esp_hal_timg
                                              idf::esp_hal_i2c idf::esp_hal_gpspi idf::esp_hal_ana_conv idf::esp_hal_pcnt idf::esp_hal_mcpwm idf::esp_hal_rmt idf::esp_hal_twai)
endif()

if(CONFIG_IDF_TARGET_ESP32C5)
# Same mechanism, same measured reason: adding esp_hal_ledc to PRIV_REQUIRES did
# NOT put hal/ledc_ll.h on this component's compile line - the build still failed
# with "pwm.c: fatal error: hal/ledc_ll.h: No such file or directory" - while
# linking the component alias does. Only LEDC is needed so far; i2c/spi/adc are
# not driven on this target yet and adding their aliases would pull in hardware
# support code that nothing here uses.
target_link_libraries(${COMPONENT_LIB} PRIVATE idf::esp_hal_ledc idf::esp_hal_i2c idf::esp_hal_ana_conv idf::esp_hal_gpspi idf::esp_hal_twai idf::esp_hal_rmt idf::esp_hal_pcnt idf::esp_hal_mcpwm idf::esp_psram idf::esp_hal_usb idf::esp_hal_dma idf::esp_hal_parlio)
endif()

if(rtt_xtensa)
# Same mechanism on ESP32-S3, and the reason the comment above gives is measured
# here too: esp_hal_ledc in PRIV_REQUIRES did NOT put hal/ledc_ll.h on the
# compile line (the -I list in build/compile_commands.json was unchanged and
# pwm.c failed with "fatal error: hal/ledc_ll.h: No such file or directory"),
# while linking the component alias fixes it.
#
# esp_hal_gpio is already pulled in as a transitive requirement of the `hal`
# umbrella, so gpio.c needs nothing extra.
target_link_libraries(${COMPONENT_LIB} PRIVATE idf::esp_hal_ledc idf::esp_hal_i2c idf::esp_hal_gpspi
                                              idf::esp_hal_ana_conv)
endif()






























