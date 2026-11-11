/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-11-03     RT-Thread    first version, ESP32-S3 Wi-Fi scan
 *
 * `s3wifiscan` - list visible 2.4 GHz APs.
 *
 * Separate from s3wifi on purpose: bringing the interface up and joining a
 * network are different questions, and a scan answers "is the radio receiving
 * anything at all" without needing credentials for a network that may not be
 * there. It is also the honest answer when a connection fails with reason 201
 * (NO_AP_FOUND) - either the SSID is hidden, or it is on 5 GHz, and the S3's
 * radio cannot see the latter.
 *
 * The whole bring-up sequence is repeated here rather than assumed, so this
 * command works whether or not s3wifi ran first. Every call in it is safe to
 * repeat: nvs_flash_init, esp_netif_init, esp_event_loop_create_default and
 * esp_wifi_init all return ESP_ERR_INVALID_STATE on a second call, which means
 * "already done" - so unlike s3wifi, this command tolerates that code, because
 * here it really is benign.
 *
 * Ported from the S31 BSP's s31wifiscan.c.
 */

/* MSH_CMD_EXPORT casts the handler to `long (*)(void)`, which is upstream's
 * design and trips -Werror=cast-function-type. Same pragma main.c carries. */
#pragma GCC diagnostic ignored "-Wcast-function-type"

#include <rtthread.h>

#include "sdkconfig.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "nvs_flash.h"

#define S3_SCAN_WAIT_MS 10000
#define S3_SCAN_MAX_AP  32

#if CONFIG_IDF_RTOS_RTTHREAD_FREERTOS_SHIM

static volatile int s_scan_done;

static void s3_scan_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    RT_UNUSED(arg);
    RT_UNUSED(data);

    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE)
    {
        s_scan_done = 1;
    }
}

static int cmd_wifiscan(int argc, char **argv)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    wifi_scan_config_t scan;
    wifi_ap_record_t  *recs;
    uint16_t           count = 0;
    int                waited = 0;
    uint16_t           i;

    RT_UNUSED(argc);
    RT_UNUSED(argv);

    s_scan_done = 0;

    /* Bring the stack up. Every call is idempotent-safe: the same sequence runs
     * in s3wifi, and re-running it here means this command works whether or not
     * s3wifi ran first. */
    if (nvs_flash_init() != ESP_OK)
    {
        rt_kprintf("s3wifiscan: nvs_flash_init failed (continuing)\n");
    }
    if (esp_netif_init() != ESP_OK)
    {
        rt_kprintf("s3wifiscan: esp_netif_init failed\n");
        return -1;
    }
    /*
     * Create the station netif - and it has to be AFTER esp_wifi_init().
     *
     * esp_netif_create_default_wifi_sta() calls
     * esp_wifi_set_default_wifi_sta_handlers(), which requires the Wi-Fi driver
     * to be initialised. Called first, the ESP_ERROR_CHECK inside it aborts the
     * boot outright:
     *
     *   ESP_ERROR_CHECK failed: esp_err_t 0x103 (ESP_ERR_INVALID_STATE)
     *   func: esp_netif_create_default_wifi_sta
     *   expression: esp_wifi_set_default_wifi_sta_handlers()
     *
     * Measured from a cold boot: `s3wifiscan` on its own reset the board every
     * time, while `s3wifi` did not - s3wifi creates the netif after the init.
     * This command created it first, and the comment below claimed that was the
     * same order s3wifi uses. It was not.
     *
     * The rest of the note explains why the netif must exist at all.
     *
     *
     * Not cosmetic: esp_wifi_start() attaches the interface to whatever netif
     * already exists, and the attach happens once. A scan-first sequence that
     * skipped this left esp_wifi running with no netif behind it - the link came
     * up (state: init -> auth -> assoc -> run, "connected ... aid = 1") and then
     * no DHCP lease ever arrived, because there was no lwIP interface to offer
     * one, and the first frame esp_wifi delivered died in esp_netif_receive():
     *
     *   Guru Meditation Error: Core 0 panic'ed (InstrFetchProhibited)
     *   PC : 0x00000000
     *   0x42053376 esp_netif_receive (esp_netif_lwip.c:1463)
     *
     * Returning NULL here is not fatal for a scan - the radio does not need a
     * netif - so it is reported and survived.
     */
    /* ESP_ERR_INVALID_STATE just means the loop already exists, which is fine
     * when s3wifi ran first. Anything else would surface at the handler
     * registration below, which is where it actually matters. */
    (void)esp_event_loop_create_default();
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, s3_scan_event,
                               RT_NULL);

    /*
     * Every bring-up call below is tolerated when it reports "already done",
     * because this command has to work in both orders:
     *
     *   s3wifiscan            first   -> the calls succeed
     *   s3wifi -> s3wifiscan  after   -> they report the state and must not fail
     *
     * Measured without the tolerance:
     *
     *   E (185489) wifi_init: Failed to init, WiFi is initialized by esp_wifi_init
     *   s3wifiscan: esp_wifi_init failed
     *
     * which reads like a defect and is really just the second caller. The
     * specific codes are ESP_ERR_WIFI_INIT_STATE from esp_wifi_init(),
     * ESP_ERR_WIFI_MODE/ESP_OK from set_mode, and ESP_ERR_WIFI_STATE from
     * esp_wifi_start(); only the init one is worth naming, the others are
     * genuinely no-ops for this purpose.
     */
    {
        esp_err_t err = esp_wifi_init(&cfg);

        /* ESP_ERR_INVALID_STATE (0x103) is what this actually returns on a
         * second call; ESP_ERR_WIFI_INIT_STATE is the WiFi-specific code the
         * same condition uses elsewhere. Both mean "already initialised", and
         * the generic one is what was measured here. */
        if (err != ESP_OK &&
            err != ESP_ERR_INVALID_STATE &&
            err != ESP_ERR_WIFI_INIT_STATE)
        {
            rt_kprintf("s3wifiscan: esp_wifi_init failed (0x%x %s)\n",
                       err, esp_err_to_name(err));
            return -1;
        }
    }

    /*
     * Create the station netif only if it does not exist yet.
     *
     * esp_netif_create_default_wifi_sta() asserts when called twice - the
     * second call reaches esp_netif_new_api() with a config whose handlers are
     * already attached:
     *
     *   E esp_netif_lwip: esp_netif_new_api: Failed to configure netif
     *   assert failed: esp_netif_create_default_wifi_sta wifi_default.c:437
     *
     * So `s3wifi` followed by `s3wifiscan` reset the board. The first version of
     * this command called it unconditionally, which was fine only when `s3wifi`
     * had not run.
     *
     * esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") is the documented way to
     * ask whether the default station netif exists. Its absence is not fatal for
     * a scan - the radio does not need a netif - so a failure to create is
     * reported and survived.
     */
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == RT_NULL)
    {
        if (esp_netif_create_default_wifi_sta() == RT_NULL)
        {
            rt_kprintf("s3wifiscan: no station netif (scan still works, DHCP will not)\n");
        }
    }

    (void)esp_wifi_set_mode(WIFI_MODE_STA);
    (void)esp_wifi_start();

    /* The station has to be started before a scan is accepted. */
    while (!s_scan_done && waited < 2000)
    {
        rt_thread_mdelay(10);
        waited += 10;
    }

    rt_memset(&scan, 0, sizeof(scan));
    scan.show_hidden = 1;
    scan.scan_type   = WIFI_SCAN_TYPE_ACTIVE;

    rt_kprintf("s3wifiscan: scanning (2.4 GHz only - the S3 has no 5 GHz radio)\n");

    s_scan_done = 0;

    /*
     * Blocking form, deliberately.
     *
     * The non-blocking form - esp_wifi_scan_start(&scan, false) plus a wait for
     * WIFI_EVENT_SCAN_DONE - was measured hanging for its full 10 s timeout with
     * no scan log at all, twice, even though the same event loop demonstrably
     * delivers WIFI_EVENT_STA_START (s3wifi reports it in 0 ms). Isolating the
     * scan from event delivery is the reliable way to find out which half is
     * wrong, and it is also the more robust form for a diagnostic command: the
     * wait happens inside the wifi driver against its own semaphore, which is
     * exactly what the FreeRTOS shim exists to provide.
     */
    {
        esp_err_t err = esp_wifi_scan_start(&scan, true);

        if (err != ESP_OK)
        {
            rt_kprintf("s3wifiscan: scan_start failed (0x%x %s)\n",
                       err, esp_err_to_name(err));
            return -1;
        }
    }

    waited = 0;
    while (!s_scan_done && waited < S3_SCAN_WAIT_MS)
    {
        rt_thread_mdelay(20);
        waited += 20;
    }

    if (!s_scan_done)
    {
        rt_kprintf("s3wifiscan: FAIL, no WIFI_EVENT_SCAN_DONE within %d ms\n",
                   S3_SCAN_WAIT_MS);
        return -1;
    }

    if (esp_wifi_scan_get_ap_num(&count) != ESP_OK)
    {
        rt_kprintf("s3wifiscan: get_ap_num failed\n");
        return -1;
    }

    if (count > S3_SCAN_MAX_AP)
    {
        count = S3_SCAN_MAX_AP;
    }

    recs = (wifi_ap_record_t *)rt_malloc(sizeof(wifi_ap_record_t) * count);
    if (recs == RT_NULL)
    {
        rt_kprintf("s3wifiscan: no memory for %d records\n", (int)count);
        return -1;
    }

    if (esp_wifi_scan_get_ap_records(&count, recs) != ESP_OK)
    {
        rt_kprintf("s3wifiscan: get_ap_records failed\n");
        rt_free(recs);
        return -1;
    }

    rt_kprintf("\n  # ch  rssi  bssid              ssid\n");
    rt_kprintf("  - --  ----  -----------------  ----\n");
    for (i = 0; i < count; i++)
    {
        rt_kprintf("  %d %2d  %4d  %02x:%02x:%02x:%02x:%02x:%02x  %s\n",
                   (int)i + 1, (int)recs[i].primary, (int)recs[i].rssi,
                   recs[i].bssid[0], recs[i].bssid[1], recs[i].bssid[2],
                   recs[i].bssid[3], recs[i].bssid[4], recs[i].bssid[5],
                   (const char *)recs[i].ssid);
    }

    rt_free(recs);

    rt_kprintf("\ns3wifiscan: %d AP(s), all on channels 1..14 = the 2.4 GHz band\n",
               (int)count);
    if (count == 0)
    {
        rt_kprintf("  Nothing heard. If the AP you want is 5 GHz-only, this board\n");
        rt_kprintf("  can never see it: the S3 radio is 2.4 GHz.\n");
    }

    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_wifiscan, s3wifiscan, list visible 2.4 GHz APs);

#else /* !CONFIG_IDF_RTOS_RTTHREAD_FREERTOS_SHIM */

static int cmd_wifiscan(int argc, char **argv)
{
    RT_UNUSED(argc);
    RT_UNUSED(argv);

    rt_kprintf("s3wifiscan: needs CONFIG_IDF_RTOS_RTTHREAD_FREERTOS_SHIM\n");
    return -1;
}
MSH_CMD_EXPORT_ALIAS(cmd_wifiscan, s3wifiscan, list visible 2.4 GHz APs);

#endif /* CONFIG_IDF_RTOS_RTTHREAD_FREERTOS_SHIM */
