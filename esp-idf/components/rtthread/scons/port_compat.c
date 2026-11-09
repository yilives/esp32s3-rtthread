/*
 * SPDX-FileCopyrightText: 2026 RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * The ESP-IDF v6.1 bt controller takes vPortYield by pointer in its OSI
 * function table.  The RT-Thread scons build runs on the FreeRTOS-Wrapper
 * kernel, where the port's yield primitive is rt_thread_yield().
 */
#include <rtthread.h>

void vPortYield(void)
{
    rt_thread_yield();
}

/*
 * Weak stubs for WPS SoftAP registrar entry points: wps.c references them
 * from wps_process_msg / wps_get_msg, but SoftAP registrar sources
 * (wps_registrar.c) are only built when CONFIG_ESP_WIFI_WPS_SOFTAP_REGISTRAR
 * is enabled.
 */
__attribute__((weak)) void *wps_registrar_get_msg(void *wps, void *op_code)
{
    (void)wps;
    (void)op_code;
    return (void *)0;
}

__attribute__((weak)) int wps_registrar_process_msg(void *wps, int op_code, const void *msg)
{
    (void)wps;
    (void)op_code;
    (void)msg;
    return 0;
}

/*
 * Weak fallback for the PHY I2C burst-length helper.  The ESP32-C6 light
 * sleep port (esp_hw_support/lowpower/port/esp32c6/sleep_modem_state.c)
 * calls it, but the RT-Thread scons build does not link the esp_phy blob and
 * never enters an ESP-IDF sleep mode.  The real definition comes from
 * esp_phy/src/phy_common.c in an IDF build and overrides this weak one.
 */
__attribute__((weak)) unsigned int phy_ana_i2c_master_burst_rf_onoff(int on)
{
    (void)on;
    return 0;
}
