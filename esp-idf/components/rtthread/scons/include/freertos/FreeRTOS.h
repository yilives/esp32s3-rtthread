/*
 * SPDX-FileCopyrightText: 2026 RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * RT-Thread scons build shim for <freertos/FreeRTOS.h>.
 *
 * The scons build of the RT-Thread ESP BSPs runs on the FreeRTOS-Wrapper
 * kernel (FreeRTOS V10.4.6) instead of the kernel that ESP-IDF vendors in
 * components/freertos/FreeRTOS-Kernel.  Upstream FreeRTOS V11 ends its
 * FreeRTOS.h by including ESP-IDF's API additions, and ESP-IDF v6.1 sources
 * rely on that: components/esp_hw_support/spi_bus_lock.c for instance calls
 * xSemaphoreCreateBinaryWithCaps() after a plain <freertos/FreeRTOS.h>.
 * The wrapper header stops earlier, so finish the job here.
 *
 * This directory has to be searched before the FreeRTOS-Wrapper include dir
 * (the root SConscript puts it first) so that #include_next finds the real
 * header.
 */
#ifndef FREERTOS_WRAPPER_SHIM_FREERTOS_H
#define FREERTOS_WRAPPER_SHIM_FREERTOS_H

/* the real (FreeRTOS-Wrapper) header, searched after this directory */
#include_next <freertos/FreeRTOS.h>

#include "freertos/idf_additions.h"

#endif /* FREERTOS_WRAPPER_SHIM_FREERTOS_H */
