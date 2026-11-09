/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-11-03     RT-Thread    first version, ESP32-S3 Wi-Fi bring-up
 *
 * `s3wifi` - bring up esp_wifi on the FreeRTOS shim.
 *
 * This is the first thing in the image that makes the shim do real work under a
 * real driver. esp_wifi_init() ends up in esp_adapter.c, whose wifi_osi_funcs_t
 * has 128 entries: it creates a task (xTaskCreatePinnedToCore), a counting
 * semaphore, mutexes, queues, an event group, arms esp_timer callbacks, and
 * reads the MAC through efuse. Every one of those lands on ft_* or on esp_timer,
 * and if any of them is wrong the failure is not a compile error - it is a hang
 * or a silent no-op.
 *
 * So the command walks the sequence one step at a time and prints the error code
 * of whichever call refused, instead of a single "PASS/FAIL". WIFI_EVENT_STA_START
 * is the interesting one: the wifi task posts it after it has come up and
 * initialised the PHY, so receiving it means a thread created through the shim
 * actually ran, blocked on a queue, and woke up with data.
 *
 * Ported from the S31 BSP's s31wifi.c. The sequence is identical because the
 * stack is identical - only the chip underneath differs, and nothing about
 * esp_wifi's own plumbing is architecture specific.
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
#include "esp_netif_ip_addr.h"
#include "nvs_flash.h"

#define S3_WIFI_WAIT_MS    5000
#define S3_WIFI_CONNECT_MS 15000
#define S3_WIFI_MAX_RETRY  3

#if CONFIG_IDF_RTOS_RTTHREAD_FREERTOS_SHIM

static volatile int s_wifi_started;
static volatile int s_wifi_connected;
static volatile int s_wifi_got_ip;
static volatile int s_wifi_fail_reason;
static esp_netif_ip_info_t s_wifi_ipinfo;

/* How many times to retry after a disconnect. The stack itself retries internally
 * for some reasons (AUTH_EXPIRE etc.); this counter is for the command's own
 * budget, so a wrong password fails in a few seconds instead of hanging for the
 * full timeout. */
static volatile int s_wifi_retries;

/* WIFI_EVENT_STA_START is delivered once per esp_wifi_init, not once per
 * esp_wifi_start(), so a second s3wifi in the same boot must not wait for it.
 * Measured: after `s3wifiscan` left the interface up, `s3wifi <ssid> <pass>`
 * printed "esp_wifi_start ok" and then timed out waiting for an event that had
 * already been delivered minutes earlier. */
static rt_bool_t s_wifi_started_once;

static void s3_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    RT_UNUSED(arg);

    if (base == WIFI_EVENT)
    {
        switch (id)
        {
        case WIFI_EVENT_STA_START:
            s_wifi_started = 1;
            s_wifi_started_once = RT_TRUE;
            break;

        case WIFI_EVENT_STA_CONNECTED:
            s_wifi_connected = 1;
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
        {
            /* `data` is a wifi_event_sta_disconnected_t; the reason code is the
             * only part worth keeping, and it is what distinguishes "wrong
             * password" from "AP not in range". */
            const wifi_event_sta_disconnected_t *d =
                (const wifi_event_sta_disconnected_t *)data;

            s_wifi_connected = 0;
            s_wifi_got_ip   = 0;
            s_wifi_fail_reason = d ? d->reason : -1;

            if (s_wifi_retries < S3_WIFI_MAX_RETRY)
            {
                s_wifi_retries++;
                esp_wifi_connect();
            }
            break;
        }

        default:
            break;
        }
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;

        if (e)
        {
            /* Keep ip, netmask and gateway: the command prints all three, and
             * copying them here is the only place they exist - the event's `data`
             * pointer is only valid for the duration of this call. */
            s_wifi_ipinfo = e->ip_info;
            s_wifi_got_ip = 1;

            /*
             * A working link restores the retry budget.
             *
             * s_wifi_retries is only ever reset at the start of cmd_wifi, so
             * without this line a device that dropped four times over its
             * lifetime would stop reconnecting forever - the counter is meant to
             * bound a *burst* of failures (a wrong password), not to be a
             * lifetime budget. Getting an IP proves the credentials and the AP
             * are fine, so the next drop starts from a full budget again.
             */
            s_wifi_retries = 0;
        }
    }
}

/*
 * Two variants on purpose.
 *
 * s3_step() is the strict one: anything but ESP_OK fails the command. It used to
 * accept ESP_ERR_INVALID_STATE as "already done" for every call, which is exactly
 * the kind of blanket tolerance that hides a real bug - see the
 * esp_event_handler_register() note below, where INVALID_STATE meant "there is no
 * default loop, so nothing was registered" and the command still printed ok.
 *
 * s3_step_idem() is for the calls that genuinely are idempotent and return
 * ESP_ERR_INVALID_STATE when this command has already run once (nvs_flash_init,
 * esp_netif_init, esp_wifi_init). Those are reported as "already done" and are
 * allowed; each one is listed explicitly at its call site.
 */
static int s3_step(const char *what, esp_err_t err)
{
    if (err == ESP_OK)
    {
        rt_kprintf("  %-26s ok\n", what);
        return 0;
    }

    rt_kprintf("  %-26s FAILED 0x%x (%s)\n", what, err, esp_err_to_name(err));
    return -1;
}

static int s3_step_idem(const char *what, esp_err_t err)
{
    if (err == ESP_OK)
    {
        rt_kprintf("  %-26s ok\n", what);
        return 0;
    }

    if (err == ESP_ERR_INVALID_STATE)
    {
        rt_kprintf("  %-26s ok (already done)\n", what);
        return 0;
    }

    rt_kprintf("  %-26s FAILED 0x%x (%s)\n", what, err, esp_err_to_name(err));
    return -1;
}

static int cmd_wifi(int argc, char **argv)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t      conf;
    esp_netif_t       *netif;
    uint8_t            mac[6];
    int                waited = 0;
    const char        *ssid = RT_NULL;
    const char        *pass = RT_NULL;
    int                connect_mode;

    /*
     * `s3wifi drop` - exercise the reconnect path deliberately.
     *
     * WIFI_EVENT_STA_DISCONNECTED calls esp_wifi_connect() while the retry
     * budget lasts, but nothing in this image ever *caused* a disconnect, so
     * that branch had only been read, never run. esp_wifi_disconnect() produces
     * the same event the AP would, so this drives the real code rather than a
     * simulation of it.
     *
     * What it proves, in order:
     *   1. the event handler really is registered and really fires
     *   2. the handler's esp_wifi_connect() is accepted from an event context
     *   3. the link comes back and DHCP renews (GOT_IP fires again)
     *
     * Step 3 matters on its own: a reconnect that associates but never gets an
     * address looks healthy in the logs and is useless in practice.
     */
    if (argc >= 2 && rt_strcmp(argv[1], "drop") == 0)
    {
        int waited_ms = 0;
        int saw_drop = 0;
        int retries_before;
        esp_err_t err;

        if (s_wifi_got_ip == 0)
        {
            rt_kprintf("s3wifi drop: no link yet - run `s3wifi <ssid> <pass>` "
                       "first\n");
            return -1;
        }

        retries_before = s_wifi_retries;

        rt_kprintf("s3wifi drop: disconnecting on purpose, retry budget at %d\n",
                   retries_before);

        err = esp_wifi_disconnect();
        if (err != ESP_OK)
        {
            rt_kprintf("s3wifi drop: esp_wifi_disconnect FAILED 0x%x (%s)\n",
                       err, esp_err_to_name(err));
            return -1;
        }

        /* Wait for the handler to notice. If it does not within a second, the
         * event never arrived and the rest of the wait would be meaningless. */
        while (waited_ms < 1000)
        {
            if (s_wifi_got_ip == 0)
            {
                saw_drop = 1;
                break;
            }

            rt_thread_mdelay(10);
            waited_ms += 10;
        }

        if (!saw_drop)
        {
            rt_kprintf("s3wifi drop: FAIL, WIFI_EVENT_STA_DISCONNECTED did not "
                       "arrive within %d ms\n", waited_ms);
            return -1;
        }

        rt_kprintf("s3wifi drop: dropped after %d ms, reason=%d, retries now "
                   "%d, waiting for reconnect...\n",
                   waited_ms, s_wifi_fail_reason, s_wifi_retries);

        /* The handler has already called esp_wifi_connect(). Give the stack the
         * same budget the initial connect gets. */
        while (s_wifi_got_ip == 0 && waited_ms < S3_WIFI_CONNECT_MS)
        {
            rt_thread_mdelay(10);
            waited_ms += 10;
        }

        if (s_wifi_got_ip == 0)
        {
            rt_kprintf("s3wifi drop: FAIL, did not reconnect within %d ms "
                       "(reason=%d, retries=%d)\n",
                       waited_ms, s_wifi_fail_reason, s_wifi_retries);
            return -1;
        }

        rt_kprintf("s3wifi drop: PASS, reconnected in %d ms\n", waited_ms);
        rt_kprintf("s3wifi drop: ip " IPSTR " mask " IPSTR " gw " IPSTR "\n",
                   IP2STR(&s_wifi_ipinfo.ip),
                   IP2STR(&s_wifi_ipinfo.netmask),
                   IP2STR(&s_wifi_ipinfo.gw));
        return 0;
    }

    if (argc >= 3)
    {
        ssid = argv[1];
        pass = argv[2];
    }
    else if (argc == 2)
    {
        ssid = argv[1];
        pass = "";
    }

    connect_mode = (ssid != RT_NULL);

    if (rt_strlen(ssid ? ssid : "") > sizeof(conf.sta.ssid) ||
        rt_strlen(pass ? pass : "") > sizeof(conf.sta.password))
    {
        rt_kprintf("s3wifi: ssid up to %u bytes, password up to %u\n",
                   (unsigned)sizeof(conf.sta.ssid),
                   (unsigned)sizeof(conf.sta.password));
        return -1;
    }

    s_wifi_started     = 0;
    s_wifi_connected   = 0;
    s_wifi_got_ip      = 0;
    s_wifi_fail_reason = 0;
    s_wifi_retries     = 0;
    rt_memset(&s_wifi_ipinfo, 0, sizeof(s_wifi_ipinfo));

    rt_kprintf("s3wifi: bringing up esp_wifi on the FreeRTOS shim\n");

    /* NVS before anything else. The PHY reads its RF calibration data from the
     * "phy" namespace and, without it, falls back to a full calibration - which
     * works but takes noticeably longer and cannot persist the result. It logs
     * "NVS has not been initialized. Call nvs_flash_init before starting
     * WiFi/BT." and carries on, so this is not fatal, only wasteful. */
    if (s3_step_idem("nvs_flash_init", nvs_flash_init()) != 0)
    {
        rt_kprintf("  (continuing: PHY will do a full calibration)\n");
    }

    /* netif next: esp_netif_create_default_wifi_sta() has to exist before the
     * wifi task starts, otherwise the interface attach in esp_wifi_start() has
     * nowhere to go. */
    if (s3_step_idem("esp_netif_init", esp_netif_init()) != 0)
    {
        return -1;
    }

    /* The default event loop, and it has to come before the handler
     * registration, not after.
     *
     * esp_event_handler_register() is a thin wrapper over the module-static
     * s_default_loop and returns ESP_ERR_INVALID_STATE when that pointer is still
     * NULL (default_event_loop.c:20). The first version of this command on the
     * S31 registered the handler first and created the loop never - so the
     * registration "succeeded" only because ESP_ERR_INVALID_STATE was being
     * accepted as a benign code, and WIFI_EVENT_STA_START had nowhere to be
     * delivered. The command would have waited out its full timeout on real
     * hardware and blamed the wifi task.
     *
     * esp_wifi also posts its events onto this loop, so without it the wifi task
     * has no consumer for STA_START regardless of how healthy it is. */
    if (s3_step_idem("event loop create default",
                     esp_event_loop_create_default()) != 0)
    {
        return -1;
    }

    netif = esp_netif_create_default_wifi_sta();
    if (netif == RT_NULL)
    {
        rt_kprintf("  %-26s FAILED\n", "create default wifi sta");
        return -1;
    }
    rt_kprintf("  %-26s ok\n", "create default wifi sta");

    /* Strict now: with the loop in place there is no benign reason for
     * INVALID_STATE here. */
    if (s3_step("event handler register",
                esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                           s3_wifi_event, RT_NULL)) != 0)
    {
        return -1;
    }

    /* IP_EVENT is a separate base with its own loop subscription. Registering
     * only WIFI_EVENT is the classic mistake: the connection succeeds, GOT_IP is
     * never delivered, and the command reports a timeout on a link that is
     * actually up. */
    if (s3_step("ip handler register",
                esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                           s3_wifi_event, RT_NULL)) != 0)
    {
        return -1;
    }

    /* This is the call that exercises the OS layer: it starts the wifi task and
     * the timer task, and allocates its queues through wifi_create_queue().
     *
     * "already done" is not just benign here, it is informative: WIFI_EVENT_STA_START
     * is delivered exactly once per esp_wifi_init, so if this returns
     * ESP_ERR_INVALID_STATE then that event has already fired - in an earlier
     * `s3wifiscan`, say - and our handler was not registered to see it. Recording
     * that now is what stops the wait below from timing out on an interface that
     * is in fact already up. Measured without it, after `s3wifiscan`:
     *
     *   esp_wifi_init              ok (already done)
     *   esp_wifi_start             ok
     *   s3wifi: FAIL, no WIFI_EVENT_STA_START within 5000 ms */
    {
        esp_err_t err = esp_wifi_init(&cfg);

        if (err == ESP_ERR_INVALID_STATE || err == ESP_ERR_WIFI_INIT_STATE)
        {
            rt_kprintf("  %-26s ok (already done)\n", "esp_wifi_init");
            s_wifi_started_once = RT_TRUE;
        }
        else if (s3_step("esp_wifi_init", err) != 0)
        {
            return -1;
        }
    }

    if (s3_step("esp_wifi_set_mode", esp_wifi_set_mode(WIFI_MODE_STA)) != 0)
    {
        return -1;
    }

    rt_memset(&conf, 0, sizeof(conf));

    if (connect_mode)
    {
        /* Credentials come from the command line and are NOT persisted: a reboot
         * forgets them. There is no provisioning path in this image, and writing
         * a password into NVS behind the user's back would be worse than not
         * storing it.
         *
         * threshold.authmode: WPA2_PSK for a non-empty password, OPEN otherwise.
         * An empty password with WPA2_PSK set makes the stack fail AUTH_EXPIRE
         * against an open AP, which reads like a wrong password. */
        rt_strncpy((char *)conf.sta.ssid, ssid, sizeof(conf.sta.ssid) - 1);
        rt_strncpy((char *)conf.sta.password, pass, sizeof(conf.sta.password) - 1);
        conf.sta.threshold.authmode =
            (pass[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        conf.sta.pmf_cfg.capable = true;

        rt_kprintf("  connecting to \"%s\" (%s)\n", ssid,
                   (pass[0] != '\0') ? "WPA2-PSK" : "open");
    }

    if (s3_step("esp_wifi_set_config",
                esp_wifi_set_config(WIFI_IF_STA, &conf)) != 0)
    {
        return -1;
    }

    /*
     * esp_wifi_start() is a no-op when the interface is already up, and
     * WIFI_EVENT_STA_START is delivered exactly once per esp_wifi_init - so a
     * second `s3wifi` in the same boot would wait out the full 5 s for an event
     * that can never arrive again. Measured, after running `s3wifiscan` first:
     *
     *     esp_wifi_start             ok
     *   s3wifi: FAIL, no WIFI_EVENT_STA_START within 5000 ms
     *
     * Treating ESP_ERR_WIFI_STATE as "already started" is the correct reading:
     * the interface is up, which is the only thing that event attests.
     */
    {
        esp_err_t err = esp_wifi_start();

        if (err == ESP_ERR_WIFI_STATE)
        {
            s_wifi_started = 1;
            rt_kprintf("  %-26s ok (already started)\n", "esp_wifi_start");
        }
        else if (s3_step("esp_wifi_start", err) != 0)
        {
            return -1;
        }
        else if (s_wifi_started_once)
        {
            /* The event fired in an earlier invocation; this start() is a no-op
             * and no second event is coming. */
            s_wifi_started = 1;
            rt_kprintf("  %-26s ok (STA_START seen earlier)\n", "sta_start");
        }
    }

    /* The wifi task posts WIFI_EVENT_STA_START once it is actually running. */
    while (s_wifi_started == 0 && waited < S3_WIFI_WAIT_MS)
    {
        rt_thread_mdelay(10);
        waited += 10;
    }

    if (s_wifi_started == 0)
    {
        rt_kprintf("s3wifi: FAIL, no WIFI_EVENT_STA_START within %d ms\n",
                   S3_WIFI_WAIT_MS);
        return -1;
    }

    if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK)
    {
        rt_kprintf("s3wifi: started in %d ms, but MAC read failed\n", waited);
        return -1;
    }

    rt_kprintf("s3wifi: STA_START in %d ms, mac %02x:%02x:%02x:%02x:%02x:%02x\n",
               waited, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    if (!connect_mode)
    {
        /* No credentials given: bring-up only. Say exactly what was and was not
         * proven, so this cannot be mistaken for a working link. */
        rt_kprintf("s3wifi: PASS (interface up, not connected - pass <ssid> <pass> "
                   "to connect)\n");
        return 0;
    }

    /* Connect *after* STA_START. esp_wifi_connect() before the event returns
     * ESP_ERR_WIFI_NOT_STARTED, which is why this is not issued earlier. */
    if (s3_step("esp_wifi_connect", esp_wifi_connect()) != 0)
    {
        return -1;
    }

    waited = 0;
    while (s_wifi_got_ip == 0 && waited < S3_WIFI_CONNECT_MS)
    {
        rt_thread_mdelay(20);
        waited += 20;

        /* Stop early once the retry budget is spent: retrying a wrong password
         * only makes the failure slower to report. */
        if (s_wifi_retries >= S3_WIFI_MAX_RETRY &&
            s_wifi_connected == 0 && s_wifi_got_ip == 0)
        {
            break;
        }
    }

    if (s_wifi_got_ip == 0)
    {
        rt_kprintf("s3wifi: FAIL after %d ms: connected=%d, retries=%d, "
                   "last reason=%d\n",
                   waited, s_wifi_connected, s_wifi_retries, s_wifi_fail_reason);
        rt_kprintf("s3wifi: reason 15 = 4WAY_HANDSHAKE_TIMEOUT, 205 = AUTH_FAIL, "
                   "both mean a wrong password;\n");
        rt_kprintf("s3wifi: 201 = NO_AP_FOUND means the SSID is not visible "
                   "(try s3wifiscan).\n");
        return -1;
    }

    rt_kprintf("s3wifi: PASS, connected to \"%s\" in %d ms\n", ssid, waited);
    rt_kprintf("s3wifi: ip " IPSTR " mask " IPSTR " gw " IPSTR "\n",
               IP2STR(&s_wifi_ipinfo.ip),
               IP2STR(&s_wifi_ipinfo.netmask),
               IP2STR(&s_wifi_ipinfo.gw));

    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_wifi, s3wifi, bring up esp_wifi on the FreeRTOS shim);

#else /* !CONFIG_IDF_RTOS_RTTHREAD_FREERTOS_SHIM */

static int cmd_wifi(int argc, char **argv)
{
    RT_UNUSED(argc);
    RT_UNUSED(argv);

    rt_kprintf("s3wifi: needs CONFIG_IDF_RTOS_RTTHREAD_FREERTOS_SHIM.\n");
    rt_kprintf("With the shim off, esp_wifi would run on the real FreeRTOS\n");
    rt_kprintf("scheduler stacked on RT-Thread, which this port does not support.\n");
    return -1;
}
MSH_CMD_EXPORT_ALIAS(cmd_wifi, s3wifi, bring up esp_wifi on the FreeRTOS shim);

#endif /* CONFIG_IDF_RTOS_RTTHREAD_FREERTOS_SHIM */
