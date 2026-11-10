/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-11-03     RT-Thread    first version, ESP32-S3 BLE bring-up
 *
 * `s3ble` - bring up the BLE controller and the NimBLE host on the shim.
 *
 * This is the Bluetooth counterpart of s3wifi: it walks the boot sequence one
 * step at a time and prints the error code of whichever call refused, instead of
 * one opaque PASS/FAIL.
 *
 * DO NOT initialise the controller here when NimBLE is enabled. nimble_port_init()
 * already does the whole controller sequence itself (nimble_port.c:288-320:
 * esp_bt_controller_init + esp_bt_controller_enable, with a deinit on failure).
 * Driving it by hand first makes that second init return ESP_ERR_INVALID_STATE
 * ("BTDM controller init failed" / 0x103), which is exactly what the first
 * version of this command did on the S31.
 *
 * So the two paths are:
 *
 *   NimBLE host:   nvs_flash_init -> nimble_port_init()      (controller + host)
 *                  -> nimble_port_freertos_init()            (host event task)
 *                  -> ble_hs sync callback                   (address read-back)
 *
 *   controller only: mem_release -> esp_bt_controller_init -> enable
 *
 * nvs_flash_init comes first because the PHY reads its RF calibration from NVS:
 * without it the log says "NVS has not been initialized. Call nvs_flash_init
 * before starting WiFi/BT" and the PHY falls back to a full calibration. That
 * works but is slower, and it cannot persist the result.
 *
 * Ported from the S31 BSP's s31ble.c. Nothing in this sequence is architecture
 * specific - the controller talks to the PHY over a register interface, and the
 * host task is just another thread the shim creates.
 */

/* MSH_CMD_EXPORT casts the handler to `long (*)(void)`, which is upstream's
 * design and trips -Werror=cast-function-type. Same pragma main.c carries. */
#pragma GCC diagnostic ignored "-Wcast-function-type"

#include <rtthread.h>
/* rt_pin_mode / rt_pin_write, for the LED characteristic the GATT write
 * handler drives. Same header main.c uses for its `s3rt led` command. */
#include <rtdevice.h>

#include "sdkconfig.h"

#if CONFIG_BT_ENABLED

#include "esp_err.h"
#include "esp_bt.h"
#include "nvs_flash.h"

#if CONFIG_BT_NIMBLE_ENABLED
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#endif

#define S3_BLE_WAIT_MS     3000

static int s3_step(const char *what, esp_err_t err)
{
    if (err == ESP_OK)
    {
        rt_kprintf("  %-28s ok\n", what);
        return 0;
    }

    rt_kprintf("  %-28s FAILED 0x%x (%s)\n", what, err, esp_err_to_name(err));
    return -1;
}

#if CONFIG_BT_NIMBLE_ENABLED

static volatile int s_ble_synced;
static volatile int s_ble_sync_rc;

/* nimble_port_init() is not idempotent: a second call makes the controller
 * init underneath it return ESP_ERR_INVALID_STATE and prints
 * "E BLE_INIT: controller init failed", which reads like a defect on a board
 * where Bluetooth is already running - measured by running `s3ble` twice. */
static rt_bool_t s_ble_up;

/* Advertising state, readable from the shell.
 *
 * s3_ble_adv_started is set by s3_ble_start_advertising() and cleared by the
 * GAP event handler when a central connects (advertising stops implicitly on
 * connect) and when it is restarted after a disconnect. The command reports it
 * so "advertising" is a measured fact rather than an assumption. */
static volatile int s_ble_adv_started;
static volatile int s_ble_adv_err;
static volatile int s_ble_conns;
static volatile int s_ble_last_disc_reason;

/* ---------------------------------------------------------------------------
 * GATT: one primary service, two characteristics
 *
 * The shape follows ESP-IDF's own NimBLE_GATT_Server example, minus the parts
 * that only exist to drive a demo:
 *
 *   0x00FF  vendor service (16-bit, so `nRF Connect` shows a name)
 *     0xFF01  READ  | 4 bytes: a counter the board increments
 *     0xFF02  WRITE | 1 byte: 0 = LED off, 1 = LED on
 *
 * A vendor 16-bit UUID rather than a SIG one on purpose: borrowing 0x180D
 * (heart rate) would make a generic client think this is a heart-rate monitor
 * and try to subscribe to measurements that do not exist.
 * ------------------------------------------------------------------------- */

static const ble_uuid16_t s_ble_svc_uuid  = BLE_UUID16_INIT(0x00FF);
static const ble_uuid16_t s_ble_chr_counter_uuid = BLE_UUID16_INIT(0xFF01);
static const ble_uuid16_t s_ble_chr_led_uuid     = BLE_UUID16_INIT(0xFF02);

static uint8_t  s_ble_counter_val[4];
static uint16_t s_ble_counter_handle;
static uint8_t  s_ble_led_val;
static uint16_t s_ble_led_handle;

/* Set when a central subscribes to the counter; only then is a notification
 * legal - sending one to an unsubscribed client returns BLE_HS_ENOTCONN. */
static uint16_t s_ble_counter_conn = BLE_HS_CONN_HANDLE_NONE;

static int s3_ble_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg);

static const struct ble_gatt_svc_def s_ble_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_ble_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid      = &s_ble_chr_counter_uuid.u,
                .access_cb = s3_ble_chr_access,
                .flags     = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_ble_counter_handle,
            },
            {
                .uuid      = &s_ble_chr_led_uuid.u,
                .access_cb = s3_ble_chr_access,
                .flags     = BLE_GATT_CHR_F_WRITE,
                .val_handle = &s_ble_led_handle,
            },
            { 0 },
        },
    },
    { 0 },
};

/* The board's user LED, same pin and polarity as main.c's `s3rt led`: 3V3 ->
 * R25 -> LED1 -> GPIO48, so the pin sinks the current and LOW lights it. */
#define S3_BLE_LED_PIN   48
#define S3_BLE_LED_ON    0
#define S3_BLE_LED_OFF   1

static int s3_ble_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint16_t uuid16 = ble_uuid_u16(ctxt->chr->uuid);

    RT_UNUSED(conn_handle);
    RT_UNUSED(attr_handle);
    RT_UNUSED(arg);

    switch (ctxt->op)
    {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        if (uuid16 == 0xFF01)
        {
            /* Publish the current counter, freshly formatted each read. */
            s_ble_counter_val[0] = (uint8_t)(s_ble_conns & 0xFF);
            s_ble_counter_val[1] = (uint8_t)(s_ble_adv_started & 0xFF);
            s_ble_counter_val[2] = (uint8_t)(s_ble_last_disc_reason & 0xFF);
            s_ble_counter_val[3] = 0;

            return os_mbuf_append(ctxt->om, s_ble_counter_val,
                                  sizeof(s_ble_counter_val)) == 0
                   ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return BLE_ATT_ERR_UNLIKELY;

    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        if (uuid16 == 0xFF02)
        {
            uint8_t val = 0;
            uint16_t len = OS_MBUF_PKTLEN(ctxt->om);

            if (len != 1)
            {
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }

            if (os_mbuf_copydata(ctxt->om, 0, 1, &val) != 0)
            {
                return BLE_ATT_ERR_UNLIKELY;
            }

            s_ble_led_val = val;

            /* rt_pin_* is safe here: the write callback runs in the NimBLE host
             * thread, not in interrupt context. */
            rt_pin_mode(S3_BLE_LED_PIN, PIN_MODE_OUTPUT);
            rt_pin_write(S3_BLE_LED_PIN, val ? S3_BLE_LED_ON : S3_BLE_LED_OFF);

            rt_kprintf("s3ble: LED %s via GATT write\n", val ? "on" : "off");
            return 0;
        }
        return BLE_ATT_ERR_UNLIKELY;

    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static void s3_ble_start_advertising(void);

static int s3_ble_gap_event(struct ble_gap_event *event, void *arg)
{
    RT_UNUSED(arg);

    switch (event->type)
    {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0)
        {
            s_ble_conns++;
            /* Advertising stops implicitly when a central connects. */
            s_ble_adv_started = 0;
            rt_kprintf("s3ble: connected, handle=%d (total %d)\n",
                       event->connect.conn_handle, s_ble_conns);
        }
        else
        {
            /* A failed connect attempt leaves the stack not advertising. */
            rt_kprintf("s3ble: connect failed, status=%d\n",
                       event->connect.status);
            s3_ble_start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        s_ble_conns = 0;
        s_ble_last_disc_reason = event->disconnect.reason;
        rt_kprintf("s3ble: disconnected, reason=%d - advertising again\n",
                   event->disconnect.reason);
        s3_ble_start_advertising();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        /* Either the duration expired or a connect raced the stop. Restarting
         * unconditionally is what the IDF example does, and it keeps the board
         * discoverable without needing a command. */
        s_ble_adv_started = 0;
        s3_ble_start_advertising();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_ble_counter_handle)
        {
            s_ble_counter_conn = event->subscribe.cur_notify
                                 ? event->subscribe.conn_handle
                                 : BLE_HS_CONN_HANDLE_NONE;
            rt_kprintf("s3ble: counter notifications %s\n",
                       event->subscribe.cur_notify ? "on" : "off");
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        rt_kprintf("s3ble: MTU updated to %d\n", event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

/*
 * Fill the advertisement and start advertising.
 *
 * Called from three places, and all three must work:
 *   - s3_ble_on_sync(), i.e. the first time the host is ready
 *   - the GAP disconnect handler, so a dropped link is re-advertised
 *   - `s3ble adv`, so the operator can restart it by hand
 *
 * Everything in here must run on the NimBLE host thread. s3_ble_on_sync and the
 * GAP handler already do; `s3ble adv` posts through ble_gap_adv_start() from the
 * shell thread, which is why that path is only used to RESTART an already
 * configured advertisement - the host task owns the initial one.
 */
static void s3_ble_start_advertising(void)
{
    struct ble_hs_adv_fields fields = {0};
    struct ble_gap_adv_params params = {0};
    const char *name;
    int rc;

    /* Flags: general discoverable, BR/EDR not supported (BLE-only board). */
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = (uint8_t)rt_strlen(name);
    fields.name_is_complete = 1;

    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.tx_pwr_lvl_is_present = 1;

    /* 0x00FF vendor service, so a scanner can see what this device offers
     * without connecting first. */
    {
        static ble_uuid16_t adv_uuids[1];

        adv_uuids[0] = s_ble_svc_uuid;
        fields.uuids16 = adv_uuids;
        fields.num_uuids16 = 1;
        fields.uuids16_is_complete = 1;
    }

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0)
    {
        s_ble_adv_err = rc;
        rt_kprintf("s3ble: adv_set_fields failed rc=%d\n", rc);
        return;
    }

    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, RT_NULL, BLE_HS_FOREVER,
                           &params, s3_ble_gap_event, RT_NULL);
    if (rc != 0)
    {
        s_ble_adv_err = rc;
        rt_kprintf("s3ble: adv_start failed rc=%d\n", rc);
        return;
    }

    s_ble_adv_started = 1;
    s_ble_adv_err = 0;
}

static void s3_ble_on_sync(void)
{
    /* Runs in the NimBLE host task once the HCI link is up. Inferring the address
     * here is the documented "the host is really talking to the controller"
     * check: it reads the controller's own identity address, so a dead HCI
     * transport shows up as a failure here rather than as silence. */
    ble_addr_t addr;

    s_ble_sync_rc = ble_hs_id_infer_auto(0, &addr.type);
    if (s_ble_sync_rc == 0)
    {
        s_ble_sync_rc = ble_hs_id_copy_addr(addr.type, addr.val, RT_NULL);
    }

    /*
     * Advertising starts HERE, not from cmd_ble.
     *
     * ble_gap_adv_start() and the GATT registration both have to run on the
     * NimBLE host task; calling them from the shell thread before the host is
     * synced returns BLE_HS_EBUSY or silently does nothing. The sync callback is
     * the first moment the stack is ready for either, which is why the IDF
     * example does its adv_init() from this exact place.
     */
    if (s_ble_sync_rc == 0)
    {
        s3_ble_start_advertising();
    }

    s_ble_synced = 1;
}

static void s3_ble_on_reset(int reason)
{
    rt_kprintf("s3ble: host reset, reason=%d\n", reason);
}

/*
 * The NimBLE host task body.
 *
 * nimble_port_freertos_init() takes the *function to run*, not a parameter - it
 * passes it straight to xTaskCreatePinnedToCore. Passing NULL instead (the first
 * version on the S31) makes ft_task_create() correctly refuse to create a task
 * with no entry point, so the host thread never exists and the sync callback
 * never fires: the command reports "host never synced" and `ps` shows no
 * nimble_host. That was a bug in the BSP file, not in the port - and a useful
 * confirmation that the shim's NULL-entry check works.
 */
static void s3_ble_host_task(void *param)
{
    RT_UNUSED(param);

    nimble_port_run();

    /* Only reached after nimble_port_stop(); keeps the thread from returning into
     * a scheduler that still has it listed. */
    nimble_port_freertos_deinit();
}

#endif /* CONFIG_BT_NIMBLE_ENABLED */

/*
 * Tear the BLE stack down from another module.
 *
 * Needed because s3wifi may be asked to connect while Bluetooth is running, and
 * that combination does not work (see docs/KNOWN-ISSUES.md). The only sequence
 * measured to work is: bring BLE down, connect Wi-Fi, bring BLE back. That has
 * to be orchestrated somewhere, and s3wifi is where the user's intent arrives.
 *
 * Returns RT_EOK when the stack is down (or was never up), -RT_ERROR when
 * nimble_port_deinit() refused. Never prints: whether that is worth a line of
 * console output is the caller's decision.
 */
rt_err_t s3ble_shutdown_quiet(void)
{
    if (!s_ble_up)
    {
        return RT_EOK;
    }

    (void)nimble_port_stop();

    if (nimble_port_deinit() != ESP_OK)
    {
        return -RT_ERROR;
    }

    s_ble_up = RT_FALSE;
    s_ble_synced = 0;
    s_ble_adv_started = 0;
    return RT_EOK;
}

rt_bool_t s3ble_is_up(void)
{
    return s_ble_up;
}

static int cmd_ble(int argc, char **argv)
{
    int waited;

#if CONFIG_BT_NIMBLE_ENABLED
    /*
     * The two state queries come FIRST, before anything that touches the stack.
     *
     * They were originally after the `s_ble_up` early return, which meant
     * `s3ble info` on an already-running stack printed the "already up" banner
     * and returned before reaching its own branch - measured on hardware.
     * A query must not have side effects and must not be shadowed by bring-up.
     */
    if (argc >= 2 && rt_strcmp(argv[1], "info") == 0)
    {
        rt_kprintf("s3ble info: up=%d synced=%d advertising=%d conns=%d "
                   "last_disc=%d adv_err=%d\n",
                   (int)s_ble_up, (int)s_ble_synced, (int)s_ble_adv_started,
                   (int)s_ble_conns, (int)s_ble_last_disc_reason,
                   (int)s_ble_adv_err);
        return 0;
    }

    /*
     * `s3ble off` - disable the controller, keep the host and the stack loaded.
     *
     * Narrower than `deinit`: this stops the radio without tearing down NimBLE.
     * If Wi-Fi works after this but not before, the conflict is the controller
     * transmitting; if it still fails, it is the controller merely existing.
     */
    if (argc >= 2 && rt_strcmp(argv[1], "off") == 0)
    {
        if (!s_ble_up)
        {
            rt_kprintf("s3ble off: stack is not up\n");
            return 0;
        }

        if (esp_bt_controller_disable() != ESP_OK)
        {
            rt_kprintf("s3ble off: esp_bt_controller_disable FAILED\n");
            return -1;
        }

        rt_kprintf("s3ble off: PASS, controller disabled (host still loaded)\n");
        return 0;
    }

    /*
     * `s3ble deinit` - tear the whole stack down.
     *
     * This exists to answer one question that `s3ble adv off` cannot: is the
     * Wi-Fi connection failure caused by something the BT controller is *doing*,
     * or by state the controller left behind when it was enabled? Stopping the
     * advertisement left Wi-Fi failing (0/1), which rules out airtime but not the
     * controller's presence.
     *
     * If Wi-Fi connects after this, the conflict is live and resumable; if it
     * still fails, the damage is already done and the only fix is ordering.
     */
    if (argc >= 2 && rt_strcmp(argv[1], "deinit") == 0)
    {
        int rc = nimble_port_stop();

        if (rc != 0)
        {
            rt_kprintf("s3ble deinit: nimble_port_stop rc=%d\n", rc);
        }

        if (nimble_port_deinit() != ESP_OK)
        {
            rt_kprintf("s3ble deinit: nimble_port_deinit FAILED\n");
            return -1;
        }

        s_ble_up = RT_FALSE;
        s_ble_synced = 0;
        s_ble_adv_started = 0;
        rt_kprintf("s3ble deinit: PASS, stack down (BLE MAC no longer on air)\n");
        return 0;
    }

    if (argc >= 2 && rt_strcmp(argv[1], "adv") == 0)
    {
        if (!s_ble_up)
        {
            rt_kprintf("s3ble adv: stack is not up - run `s3ble` first\n");
            return -1;
        }

        /*
         * `s3ble adv off` - stop advertising but leave the controller running.
         *
         * Needed to tell "BLE is on the air" apart from "the BLE controller
         * exists". Those are different states and, on this port, only the first
         * one breaks Wi-Fi association: connecting while BLE advertises fails
         * 0/4 (`deauth reason:1` after `run -> init`), and the open question is
         * whether it is the radio traffic or merely the controller being enabled.
         *
         * Stopping the advertisement is the cheapest way to separate the two:
         * the controller, the host task and the coex registration all stay up.
         */
        if (argc >= 3 && rt_strcmp(argv[2], "off") == 0)
        {
            int rc;

            if (!s_ble_adv_started)
            {
                rt_kprintf("s3ble adv: not advertising\n");
                return 0;
            }

            rc = ble_gap_adv_stop();
            if (rc != 0)
            {
                rt_kprintf("s3ble adv off: FAIL, rc=%d\n", rc);
                return -1;
            }

            s_ble_adv_started = 0;
            rt_kprintf("s3ble adv: PASS, advertising stopped "
                       "(controller still up)\n");
            return 0;
        }

        if (s_ble_adv_started)
        {
            rt_kprintf("s3ble adv: already advertising\n");
            return 0;
        }

        s3_ble_start_advertising();

        if (!s_ble_adv_started)
        {
            rt_kprintf("s3ble adv: FAIL, rc=%d\n", s_ble_adv_err);
            return -1;
        }

        rt_kprintf("s3ble adv: PASS, advertising again\n");
        return 0;
    }
#endif /* CONFIG_BT_NIMBLE_ENABLED */

    rt_kprintf("s3ble: bringing up the BLE controller on the FreeRTOS shim\n");

    /* PHY calibration lives in NVS. Not fatal - the PHY falls back to a full
     * calibration - but reported, because the fallback is slow and cannot be
     * persisted. */
    if (s3_step("nvs_flash_init", nvs_flash_init()) != 0)
    {
        rt_kprintf("  (continuing: PHY will do a full calibration)\n");
    }

#if CONFIG_BT_NIMBLE_ENABLED

    /*
     * nimble_port_init() initialises AND enables the controller itself. Calling
     * esp_bt_controller_init() before it makes that second init return
     * ESP_ERR_INVALID_STATE (0x103), which is what the first version of this
     * command hit: "BLE_INIT: controller init failed" immediately after the
     * controller had already come up.
     *
     * It is also not safe to call twice for the same reason, so a second
     * invocation reports the state it is already in instead of tearing the stack
     * down and failing.
     */
    if (s_ble_up)
    {
        rt_kprintf("  %-28s ok (already running)\n", "nimble_port_init");

        {
            uint8_t addr[6] = {0};

            (void)ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, addr, RT_NULL);

            rt_kprintf("s3ble: PASS (already up), addr %02x:%02x:%02x:%02x:%02x:%02x\n",
                       addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
        }
        return 0;
    }

    s_ble_synced  = 0;
    s_ble_sync_rc = -1;

    /* The callbacks have to be installed before the host task starts, or the sync
     * event fires into a NULL pointer. */
    ble_hs_cfg.sync_cb  = s3_ble_on_sync;
    ble_hs_cfg.reset_cb = s3_ble_on_reset;

    if (s3_step("nimble_port_init", nimble_port_init()) != 0)
    {
        return -1;
    }

    /*
     * GAP and GATT registration, in this order, before the host task exists.
     *
     * ble_svc_gap_init() installs the GAP service (device name, appearance) and
     * ble_svc_gatt_init() the mandatory GATT service; both are prerequisites for
     * ble_gatts_add_svcs(), which is what makes the vendor service reachable.
     * Doing this after nimble_port_freertos_init() instead would race the host
     * task and the services would be missing from the attribute table.
     *
     * ble_svc_gap_device_name_set() is what a scanner sees as the device name;
     * without it the name is the NimBLE default ("nimble") on every board.
     */
    {
        int rc;

        ble_svc_gap_init();
        ble_svc_gatt_init();

        (void)ble_svc_gap_device_name_set("s3-rtt");

        rc = ble_gatts_count_cfg(s_ble_svcs);
        if (s3_step("ble_gatts_count_cfg", rc) != 0)
        {
            return -1;
        }

        rc = ble_gatts_add_svcs(s_ble_svcs);
        if (s3_step("ble_gatts_add_svcs", rc) != 0)
        {
            return -1;
        }
    }

    nimble_port_freertos_init(s3_ble_host_task);
    rt_kprintf("  %-28s ok\n", "nimble_port_freertos_init");

    waited = 0;
    while (s_ble_synced == 0 && waited < S3_BLE_WAIT_MS)
    {
        rt_thread_mdelay(10);
        waited += 10;
    }

    if (s_ble_synced == 0)
    {
        rt_kprintf("s3ble: FAIL, host never synced within %d ms "
                   "(no HCI response from the controller)\n", S3_BLE_WAIT_MS);
        return -1;
    }

    if (s_ble_sync_rc != 0)
    {
        rt_kprintf("s3ble: FAIL, host synced but the address read failed (%d)\n",
                   s_ble_sync_rc);
        return -1;
    }

    {
        uint8_t addr[6] = {0};

        (void)ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, addr, RT_NULL);

        rt_kprintf("s3ble: PASS, host synced in %d ms, addr %02x:%02x:%02x:%02x:%02x:%02x\n",
                   waited, addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    }

    /*
     * Advertising is started from s3_ble_on_sync(), which runs on the host task;
     * this only reports whether it happened, with a short grace period because
     * the sync callback and this thread race by design.
     */
    waited = 0;
    while (s_ble_adv_started == 0 && s_ble_adv_err == 0 && waited < 1000)
    {
        rt_thread_mdelay(10);
        waited += 10;
    }

    s_ble_up = RT_TRUE;

    if (s_ble_adv_started)
    {
        rt_kprintf("s3ble: advertising as \"s3-rtt\" (service 0x00FF, "
                   "chr 0xFF01 read, 0xFF02 write)\n");
    }
    else
    {
        rt_kprintf("s3ble: FAIL, synced but not advertising (rc=%d)\n",
                   s_ble_adv_err);
        return -1;
    }

    return 0;
#else
    {
        esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();

        /* BLE-only builds: the BR/EDR pools are never allocated, because
         * CONFIG_BT_CTRL_BREDR_ENABLE=n keeps libbredr_app.a out of the image. */
        (void)s3_step("mem_release (BR/EDR pools)",
                      esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

        if (s3_step("esp_bt_controller_init", esp_bt_controller_init(&cfg)) != 0)
        {
            return -1;
        }

        if (s3_step("esp_bt_controller_enable(BLE)",
                    esp_bt_controller_enable(ESP_BT_MODE_BLE)) != 0)
        {
            return -1;
        }

        rt_kprintf("s3ble: PASS, controller only (CONFIG_BT_NIMBLE_ENABLED is off)\n");
        return 0;
    }
#endif /* CONFIG_BT_NIMBLE_ENABLED */
}
MSH_CMD_EXPORT_ALIAS(cmd_ble, s3ble, bring up the BLE controller and NimBLE host);

#else /* !CONFIG_BT_ENABLED */

static int cmd_ble(int argc, char **argv)
{
    RT_UNUSED(argc);
    RT_UNUSED(argv);

    rt_kprintf("s3ble: Bluetooth is not compiled in (CONFIG_BT_ENABLED is off).\n");
    rt_kprintf("Enable it in sdkconfig.defaults: CONFIG_BT_ENABLED=y and\n");
    rt_kprintf("CONFIG_BT_NIMBLE_ENABLED=y, then rebuild.\n");
    return -1;
}
MSH_CMD_EXPORT_ALIAS(cmd_ble, s3ble, bring up the BLE controller and NimBLE host);

#endif /* CONFIG_BT_ENABLED */
