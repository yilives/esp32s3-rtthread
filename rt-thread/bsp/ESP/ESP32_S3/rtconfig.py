# Toolchain and link configuration for the ESP32-S3 (Xtensa LX7) BSP.
#
# The toolchain is Espressif's xtensa-esp-elf, which ships Xtensa's own GCC plus
# the per-chip `xtensa-config` headers. Those headers (<xtensa_context.h>,
# <xtensa_api.h>, <xtensa/config/core.h>) are generated per chip and cannot be
# carried in libcpu/, which is why ESP32_S3.idf_path has to point at an ESP-IDF
# checkout: that is where the S3's copy lives.

import os
import sys

BSP_ROOT = os.path.dirname(os.path.abspath(__file__))
RTT_ROOT = os.getenv('RTT_ROOT') or os.path.normpath(os.path.join(BSP_ROOT, '..', '..', '..'))
sys.path = sys.path + [os.path.join(RTT_ROOT, 'tools')]
from env_package import find_package_path

# toolchains options
ARCH        = 'xtensa'
CPU         = 'esp32s3'
CROSS_TOOL  = 'gcc'

if os.getenv('RTT_CC'):
    CROSS_TOOL = os.getenv('RTT_CC')

if CROSS_TOOL == 'gcc':
    PLATFORM    = 'gcc'
    EXEC_PATH   = r'~/.espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin'

if os.getenv('RTT_EXEC_PATH'):
    EXEC_PATH = os.getenv('RTT_EXEC_PATH')
EXEC_PATH = os.path.expanduser(EXEC_PATH)

BUILD = 'debug'

if PLATFORM == 'gcc':
    PREFIX = 'xtensa-esp32s3-elf-'
    CC = PREFIX + 'gcc'
    CXX = PREFIX + 'g++'
    AS = PREFIX + 'gcc'
    AR = PREFIX + 'ar'
    LINK = PREFIX + 'gcc'
    TARGET_EXT = 'elf'
    SIZE = PREFIX + 'size'
    OBJDUMP = PREFIX + 'objdump'
    OBJCPY = PREFIX + 'objcopy'
    STRIP = PREFIX + 'strip'

    # -mlongcalls: this chip's IROM is only reachable through the 0x42000000
    # window and every call has to go through a literal. Building without it
    # produces "call target out of range" at link time, not silently wrong code.
    DEVICE = ' -mlongcalls -mserialize-volatile -mtext-section-literals'

    CFLAGS = DEVICE + ' -std=gnu17 -Wall -Wno-unused-parameter'
    CFLAGS += ' -gdwarf-4 -ggdb -Og'
    AFLAGS = ' -c' + DEVICE + ' -x assembler-with-cpp'
    CXXFLAGS = CFLAGS

    # ESP-IDF is only consulted for the chip's ROM symbols, the peripheral
    # register definitions and the xtensa-config headers; the kernel and the
    # drivers come from this tree.
    #
    # It is resolved in this order, and the order matters:
    #
    #   1. RTT_IDF_PATH, then IDF_PATH - an explicit answer, which is what a
    #      developer working in a checkout wants and what CI sets.
    #   2. find_package_path(), i.e. a `packages/ESP-IDF-latest` installed by
    #      `pkgs --update` - the RT-Thread way, and what the other ESP BSPs use.
    #   3. The fallback path find_package_path() returns when nothing is
    #      installed. It does not exist, and the compiler's error then names the
    #      first missing header, so a reader learns what to install.
    #
    # Measured while wiring this up: with only step 2 in place the resolved path
    # was <bsp>/packages/ESP-IDF-latest, which does not exist, and the build
    # failed on <soc/soc.h> with no hint that the cause was a missing package.
    idf_candidate = os.getenv('RTT_IDF_PATH') or os.getenv('IDF_PATH')
    if idf_candidate and os.path.isdir(idf_candidate):
        idf_path = os.path.abspath(idf_candidate)
    else:
        idf_path = os.path.abspath(
            find_package_path(BSP_ROOT, 'ESP-IDF-latest', relative_to=None, pathsep='/'))

    LFLAGS = DEVICE + ' -Wl,--cref -Wl,--gc-sections,-Map=rtthread.map,-cref,-u,_start'
    LFLAGS += ' -T board/link.lds'
    LFLAGS += ' -T ' + idf_path + '/components/esp_rom/esp32s3/ld/esp32s3.rom.ld'
    LFLAGS += ' -T ' + idf_path + '/components/esp_rom/esp32s3/ld/esp32s3.rom.api.ld'
    LFLAGS += ' -T ' + idf_path + '/components/esp_rom/esp32s3/ld/esp32s3.rom.libgcc.ld'
    LFLAGS += ' -T ' + idf_path + '/components/esp_rom/esp32s3/ld/esp32s3.rom.newlib.ld'
    LFLAGS += ' -T ' + idf_path + '/components/soc/esp32s3/ld/esp32s3.peripherals.ld'

    POST_ACTION = OBJCPY + ' -Oihex $TARGET rtthread.hex\n' + SIZE + ' $TARGET \n'


def dist_handle(BSP_ROOT, dist_dir):
    import sys
    sys.path.append(os.path.join(os.path.dirname(BSP_ROOT), 'tools'))
    from sdk_dist import dist_do_building
    dist_do_building(BSP_ROOT, dist_dir)
