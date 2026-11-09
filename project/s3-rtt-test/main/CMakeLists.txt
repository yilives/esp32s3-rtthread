# The application component. main.c is a bring-up image: it proves the kernel and
# the on-chip peripherals. The wireless commands live in their own files because
# they pull in the whole Wi-Fi / BLE stack, and one file that is half-compiled
# depending on sdkconfig is harder to reason about than three that are.
#
# PRIV_REQUIRES has to name every component whose *headers* these files include,
# not just the ones whose code they call:
#
#   rtthread      the kernel
#   esp_rom       esp_rom_sys.h (esp_rom_delay_us)
#   heap          esp_heap_caps.h (MALLOC_CAP_SPIRAM, heap_caps_malloc)
#   esp_psram     esp_psram_is_initialized() / esp_psram_get_size()
#   esp_hal_ledc  soc/ledc_struct.h, for the PWM register read-back
#   esp_wifi / esp_netif / esp_event / nvs_flash / lwip / esp_coex / esp_phy / bt
#                 the wireless stack
#
# The wireless components are listed unconditionally. A conditional REQUIRES
# looked tidier, but CONFIG_* is not reliably visible when this file is
# evaluated, and the block then silently evaluated false and main.c failed to
# find esp_event.h. Listing them always is safe: with the CONFIG_* off, the code
# inside those files is compiled out and --gc-sections drops the components.
#
# WHOLE_ARCHIVE is the part that actually makes the commands appear.
#
# Nothing references cmd_wifi()/cmd_ble()/cmd_wifiscan(): the msh command table
# is section-based (FSymTab), so the linker has no reason to pull these objects
# out of libmain.a, and without a hint the commands would silently never appear
# in the shell. `-u cmd_wifi` does not work for two separate reasons, both
# measured:
#
#   1. the handlers are `static`, and `-u` creates an *external* undefined
#      reference that a local symbol cannot satisfy - ld accepts it silently
#      (`-u` means "force", not "require") and leaves it dangling;
#   2. even for an external symbol, IDF appends a component's own interface
#      libraries at the end of the link line, so the archive has already been
#      scanned by the time the symbol becomes undefined.
#
# Forcing every object in is cheaper to reason about than ordering tricks, and
# there are only four.
idf_component_register(SRCS "main.c" "s3wifi.c" "s3wifiscan.c" "s3ble.c" "s3net.c"
                                     "s3wdt.c" "s3fs.c" "s3fsbench.c" "s3ulp.c"
                       INCLUDE_DIRS "."
                       # ulp is a plain REQUIRES, not PRIV_REQUIRES.
                       # ulp_embed_binary() passes this component's
                       # INTERFACE_INCLUDE_DIRECTORIES to the nested ULP build, and
                       # PRIV_REQUIRES does not populate that property - so with ulp
                       # as PRIV the ULP program failed to find ulp_riscv.h,
                       # ulp_riscv_lock.h, hal/touch_sens_types.h and hal/i2c_ll.h
                       # (all four are compiled into every ULP image by
                       # ulp_apply_default_sources). Measured.
                       REQUIRES ulp
                       PRIV_REQUIRES rtthread esp_rom heap esp_psram esp_hal_ledc
                                     esp_hal_wdt esp_hw_support
                                     fatfs wear_levelling vfs
                                     esp_wifi esp_netif esp_event nvs_flash
                                     lwip esp_coex esp_phy bt
                       WHOLE_ARCHIVE)

# MSH_CMD_EXPORT casts the command function to `long (*)(void)`, which is the
# upstream finsh ABI; silence the cast warning instead of touching the macro.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-cast-function-type)

# Wear levelling relocates a whole 4 KB block by copying it through a scratch
# buffer, in chunks of WL_DEFAULT_TEMP_BUFF_SIZE (32 by default) - i.e. 128
# read+write pairs per relocation. Measured: 136 relocations of a 1 MB partition
# produced 1168 flash writes, which is 8 per relocation and matches a 512-byte
# scratch buffer, not 128 - but the constant is overridable and the default is
# still small enough to matter on other configurations.
#
# Raising it to 4096 makes each relocation a single read and a single write. The
# buffer is per WL instance, allocated from internal RAM, so this costs 4 KB once.
# The IDF file is not modified: WL_DEFAULT_TEMP_BUFF_SIZE is wrapped in #ifndef,
# which is exactly the seam meant for this.
idf_component_get_property(wl_lib wear_levelling COMPONENT_LIB)
if(wl_lib)
    target_compile_definitions(${wl_lib} PRIVATE WL_DEFAULT_TEMP_BUFF_SIZE=4096)
endif()

# ULP-RISC-V co-processor program.
#
# ulp_embed_binary() runs a nested CMake project with the RISC-V toolchain
# (riscv32-esp-elf-gcc, not the Xtensa one this component uses), links the result
# against ulp_riscv.ld so it lands in RTC slow memory, converts it to a binary, and
# embeds it in the application image. The generated ulp_main.h declares the
# program's exported globals; s3ulp.c reads them to prove the co-processor ran.
#
# This is the whole "port": the ULP has its own ISA and its own toolchain and does
# not run under RT-Thread, so no kernel or libcpu code is involved on that side.
if(CONFIG_ULP_COPROC_ENABLED AND CONFIG_ULP_COPROC_TYPE_RISCV)
    set(ulp_app_name ulp_main)
    set(ulp_riscv_sources "ulp/s3ulp_main.c")
    set(ulp_exp_dep_srcs "s3ulp.c")
    ulp_embed_binary(${ulp_app_name} "${ulp_riscv_sources}" "${ulp_exp_dep_srcs}")
endif()

# The filesystem benchmark counts flash operations, which is the only way to tell
# "250 ms because 60 sectors were programmed" apart from "250 ms because one block
# was relocated". --wrap redirects the two entry points every layer above
# wear_levelling funnels through, without touching any IDF file.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_flash_write"
    "-Wl,--wrap=esp_flash_erase_region")
