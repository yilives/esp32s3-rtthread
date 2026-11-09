# Include path helper for the ESP32-S3 BSP's scons build.
#
# The board drivers in board/ are thin layers over ESP-IDF's LL headers
# (<soc/...>, <hal/...>, <esp_rom_...>, <xtensa_...>). Under ESP-IDF those
# directories arrive on the compile line automatically; under scons nothing
# computes them, so they have to be named.
#
# The list is DERIVED, not written out, and the derivation is deliberately
# narrow: for each component, only the shared `include` directory and the
# esp32s3-specific ones are added. That matters because a naive recursive sweep
# over components/*/include finds 222 directories - every other chip's
# esp32c3/, esp32c6/, ... variants included - and whichever one sorts first wins.
# A build that silently compiles against the C3's register map is much worse
# than one that fails with a named missing header.
#
# A component in the list that does not exist contributes nothing, so a header
# that IDF moves shows up as `fatal error: <name>: No such file or directory`
# with the offending name in it - which is a far better starting point than a
# stale hand-maintained path list.

import os


# Components whose headers the board layer reaches. Adding a driver that pulls in
# a new peripheral means adding its esp_hal_* component here.
_IDF_COMPONENTS = [
    # SoC register definitions and capability macros.
    'soc',
    # The HAL umbrella plus the per-peripheral LL implementations. In IDF 6.x
    # the LL headers moved into separate esp_hal_* components.
    'hal',
    'esp_hal_gpio',
    'esp_hal_uart',
    'esp_hal_ledc',
    'esp_hal_i2c',
    'esp_hal_gpspi',
    'esp_hal_ana_conv',
    'esp_hal_regi2c',
    'esp_hal_systimer',
    # ROM entry points (esp_rom_printf, esp_rom_delay_us, the interrupt matrix
    # router) and the per-chip symbol list.
    'esp_rom',
    # esp_private/periph_ctrl.h, esp_private/esp_clk.h, the SAR power control,
    # the interrupt allocator.
    'esp_hw_support',
    'esp_common',
    'esp_system',
    # Newlib / picolibc compatibility shims.
    'esp_libc',
    # xtensa_context.h, xtensa_api.h, <xtensa/config/core.h>. These are
    # generated per chip and therefore cannot live in libcpu/xtensa.
    'xtensa',
    # FreeRTOS types only: the board layer includes <freertos/FreeRTOS.h> and
    # <freertos/portmacro.h> for FreeRTOSBaseType_t and the portMUX helpers that
    # IDF components exchange with the kernel. No FreeRTOS kernel code is built.
    'freertos',
    'heap',
    'log',
]

# Subdirectories inside a component, relative to it, checked in this order.
# `include` is the shared half; `esp32s3/include` and `esp32s3` are the chip's
# own; `esp32s3/register` holds the register-struct headers.
_IDF_SUBDIRS = [
    'include',
    'platform_include',
    'platform_port/include',
    'esp32s3/include',
    'esp32s3',
    'esp32s3/register',
    # FreeRTOS keeps its port and config headers in these, and the board layer
    # needs them for portmacro.h.
    'FreeRTOS-Kernel/include',
    'FreeRTOS-Kernel/portable/xtensa/include',
    'FreeRTOS-Kernel/portable/xtensa/include/freertos',
    'config/include',
    'config/include/freertos',
    'config/xtensa/include',
    'esp_additions/include',
    'port/esp32s3/include',
    # The component root: port_systick.h (vPortSetupTimer) sits there
    # rather than under an include/ directory.
    '',
]


def idf_include_paths(idf_path):
    """Return the existing IDF include directories, in a stable order.

    idf_path points at an ESP-IDF checkout (the BSP resolves it from IDF_PATH,
    RTT_IDF_PATH or an installed package). Paths that do not exist are skipped.
    """
    paths = []

    if not idf_path or not os.path.isdir(idf_path):
        return paths

    for component in _IDF_COMPONENTS:
        base = os.path.join(idf_path, 'components', component)
        if not os.path.isdir(base):
            continue

        for sub in _IDF_SUBDIRS:
            candidate = os.path.normpath(os.path.join(base, sub))
            if os.path.isdir(candidate) and candidate not in paths:
                paths.append(candidate)

    return paths
