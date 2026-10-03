/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * BLE Toolbox — host side: the NimBLE host bound to the co-processor's controller.
 *
 * See ble_toolbox_host.h for scope. The short version: the P4 has no radio, so this
 * runs NimBLE against the C6 over esp-hosted's HCI transport, and this file owns that
 * host and the scanning half of the toolbox.
 */

#include "ble_toolbox_host.h"

#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_hosted_bt_host_stack.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_sm.h"
#include "host/util/util.h"
#include "store/config/ble_store_config.h"

static const char *TAG = "tb_ble";

/* Defaults. Chosen to be a scan that hears everything without disturbing anything:
 * a wide window, passive (no scan requests, so no advertiser is prompted to answer),
 * and duplicate filtering so the table is devices rather than packets. */
#define TB_BLE_SCAN_INTERVAL_MS   100
#define TB_BLE_SCAN_WINDOW_MS     100
/* The controller stops a scan after about 10 s; this caps how many times it is restarted.
 * 5 minutes of continuous scanning per Start press. */
#define TB_BLE_MAX_RESUMES        30

#define TB_BLE_SCAN_DURATION_MS   5000       /* controller auto-stops (extended scan Scan_Duration) */

typedef struct {
    bool                             initialized;
    bool                             ready;       /* NimBLE synced with the controller */
    volatile bool                    scanning;
    /* Whether a scan is WANTED, as distinct from whether one is running.
     *
     * These differ because the controller stops a scan on its own: measured here, a scan
     * asked for with duration 0 - "until disabled" - ends after about 10.2 seconds with
     * BLE_GAP_EVENT_DISC_COMPLETE, having heard 747 advertisements and then gone quiet.
     * The host passes duration straight through to the HCI command, so the stopping is the
     * controller's decision, not a timeout in this code.
     *
     * A watcher task restarts the scan while it is still wanted, which also covers the
     * controller resetting under us. The alternative - leaving it stopped - means a user who
     * presses Start sees a list that stops updating after ten seconds with nothing on screen
     * saying why. */
    volatile bool                    want_scan;
    ble_toolbox_host_callbacks_t     cbs;
    void                            *user;
    ble_toolbox_scan_params_t        params;
    uint32_t                         adv_count;

    /* How many times the scan has been restarted in this session. Bounded because a scan
     * that never rests is the workload that correlates with the controller dying. */
    uint32_t                         resume_count;

    /* Backoff for resumes that FAIL. A loop whose only exit is success has no exit, and
     * this one filled the console with an identical failure once per second. */
    uint32_t                         resume_failures;
    uint32_t                         resume_backoff_ms;
    uint32_t                         resume_at_ms;

    /* Connection (GATT client). */
    uint16_t                         conn_handle;
    ble_toolbox_conn_state_t         conn_state;

    /* Pending pairing-code request. */
    uint16_t                         pend_conn_handle;
    uint8_t                          pend_action;
    bool                             pend_passkey;
} tb_ble_state_t;

static tb_ble_state_t s_ble;

/* Signalled when a scan stops (DISC_COMPLETE). The connect path blocks on this instead of
 * busy-polling s_ble.scanning: a polling loop was measured to stop the controller's
 * Scan_Duration timeout from ever arriving, while a blocked wait does not disturb it. */
static SemaphoreHandle_t s_scan_done;

/* Resumes a scan the controller stopped, and the sync wait that backs it. Defined below
 * with the scanning code; declared here because init() starts it. */
static void tb_ble_scan_watchdog(void *arg);

/* ---- Address helpers ------------------------------------------------------ */

/* The controller reports addresses least-significant byte first. Printing them in
 * that order would produce an address that matches nothing anyone sees elsewhere, so
 * they are reversed here, and the address type is noted because a random address and
 * a public one look identical otherwise. */
static void format_addr(const uint8_t *addr, bool random, char *out, size_t out_len)
{
    snprintf(out, out_len, "%02x:%02x:%02x:%02x:%02x:%02x%s",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0],
             random ? " (rnd)" : "");
}

/* The top two bits of the most-significant byte classify a BLE address:
 *   00 = public (or non-resolvable private), 01 = RPA, 11 = static random.
 * Some controllers report a stale Address_Type in extended advertising reports, so
 * derive it from the value itself. */
static bool addr_is_random(const uint8_t addr[6])
{
    return (addr[5] & 0xC0) != 0x00;
}

/* ---- Advertisement callback ---------------------------------------------- */

static int tb_ble_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        const struct ble_gap_disc_desc *d = &event->disc;

        s_ble.adv_count++;

        /* The listing form. */
        if (s_ble.cbs.on_adv != NULL) {
            ble_toolbox_adv_t adv;
            memset(&adv, 0, sizeof(adv));

            memcpy(adv.addr, d->addr.val, sizeof(adv.addr));
            format_addr(d->addr.val, addr_is_random(d->addr.val),
                        adv.addr_str, sizeof(adv.addr_str));
            adv.rssi = d->rssi;
            adv.connectable = (d->event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
                               d->event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND);
            adv.scannable = (d->event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
                             d->event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_IND);
            adv.directed = (d->event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND);
            adv.random_addr = (d->addr.type != BLE_ADDR_PUBLIC);

            struct ble_hs_adv_fields fields;
            if (ble_hs_adv_parse_fields(&fields, d->data, d->length_data) == 0 &&
                fields.name != NULL && fields.name_len > 0) {
                const size_t n = (fields.name_len < sizeof(adv.name) - 1)
                                 ? fields.name_len : sizeof(adv.name) - 1;
                memcpy(adv.name, fields.name, n);
                adv.name[n] = '\0';
            }

            s_ble.cbs.on_adv(&adv, s_ble.user);
        }

        /* The raw form, for the observer. Truncated rather than dropped when an
         * advertisement exceeds the buffer: a partial record is still evidence,
         * silence is not. */
        if (s_ble.cbs.on_raw != NULL) {
            ble_toolbox_raw_adv_t raw;
            memset(&raw, 0, sizeof(raw));

            memcpy(raw.addr, d->addr.val, sizeof(raw.addr));
            format_addr(d->addr.val, addr_is_random(d->addr.val),
                        raw.addr_str, sizeof(raw.addr_str));
            raw.rssi = d->rssi;
            raw.adv_type = d->event_type;
            raw.data_len = (d->length_data < sizeof(raw.data))
                           ? d->length_data : (uint8_t)sizeof(raw.data);
            memcpy(raw.data, d->data, raw.data_len);

            s_ble.cbs.on_raw(&raw, s_ble.user);
        }
        break;
    }

    case BLE_GAP_EVENT_EXT_DISC: {
        /* Extended-scan advertising report (BLE 5.0). Mirrors BLE_GAP_EVENT_DISC but
         * derives the flags from the report-properties bitmask instead of a legacy PDU
         * type. */
        const struct ble_gap_ext_disc_desc *d = &event->ext_disc;

        s_ble.adv_count++;

        if (s_ble.cbs.on_adv != NULL) {
            ble_toolbox_adv_t adv;
            memset(&adv, 0, sizeof(adv));

            memcpy(adv.addr, d->addr.val, sizeof(adv.addr));
            format_addr(d->addr.val, addr_is_random(d->addr.val),
                        adv.addr_str, sizeof(adv.addr_str));
            adv.rssi = d->rssi;
            adv.connectable = (d->props & BLE_HCI_ADV_CONN_MASK) != 0;
            adv.scannable   = (d->props & BLE_HCI_ADV_SCAN_MASK) != 0;
            adv.directed    = (d->props & BLE_HCI_ADV_DIRECT_MASK) != 0;
            adv.random_addr = (d->addr.type != BLE_ADDR_PUBLIC);

            struct ble_hs_adv_fields fields;
            if (ble_hs_adv_parse_fields(&fields, d->data, d->length_data) == 0 &&
                fields.name != NULL && fields.name_len > 0) {
                const size_t n = (fields.name_len < sizeof(adv.name) - 1)
                                 ? fields.name_len : sizeof(adv.name) - 1;
                memcpy(adv.name, fields.name, n);
                adv.name[n] = '\0';
            }

            s_ble.cbs.on_adv(&adv, s_ble.user);
        }

        if (s_ble.cbs.on_raw != NULL) {
            ble_toolbox_raw_adv_t raw;
            memset(&raw, 0, sizeof(raw));

            memcpy(raw.addr, d->addr.val, sizeof(raw.addr));
            format_addr(d->addr.val, addr_is_random(d->addr.val),
                        raw.addr_str, sizeof(raw.addr_str));
            raw.rssi = d->rssi;
            raw.adv_type = ((d->props & BLE_HCI_ADV_LEGACY_MASK) != 0)
                           ? d->legacy_event_type : BLE_HCI_ADV_RPT_EVTYPE_NONCONN_IND;
            raw.data_len = (d->length_data < sizeof(raw.data))
                           ? d->length_data : (uint8_t)sizeof(raw.data);
            memcpy(raw.data, d->data, raw.data_len);

            s_ble.cbs.on_raw(&raw, s_ble.user);
        }
        break;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        /* A timed scan ran out, or the controller stopped it. The state has to follow,
         * or is_scanning() would keep claiming a scan that is over. */
        ESP_LOGI(TAG, "disc-complete event: scanning=%d adv=%u",
                 (int)s_ble.scanning, (unsigned)s_ble.adv_count);
        if (s_ble.scanning) {
            s_ble.scanning = false;
            if (s_scan_done != NULL) {
                xSemaphoreGive(s_scan_done);
            }
            ESP_LOGI(TAG, "scan complete, %u advertisements", (unsigned)s_ble.adv_count);
            if (s_ble.cbs.on_scan_state != NULL) {
                s_ble.cbs.on_scan_state(false, ESP_OK, s_ble.user);
            }
        }
        break;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_ble.conn_handle = event->connect.conn_handle;
            s_ble.conn_state = BLE_TOOLBOX_CONN_CONNECTED;
            ESP_LOGI(TAG, "connected (handle %u)", (unsigned)event->connect.conn_handle);
            if (s_ble.cbs.on_conn_state != NULL) {
                s_ble.cbs.on_conn_state(BLE_TOOLBOX_CONN_CONNECTED, ESP_OK, s_ble.user);
            }
        } else {
            s_ble.conn_state = BLE_TOOLBOX_CONN_DISCONNECTED;
            ESP_LOGW(TAG, "connect failed: status=%d", event->connect.status);
            if (s_ble.cbs.on_conn_state != NULL) {
                /* Carry the BLE status up so the UI can show it. */
                s_ble.cbs.on_conn_state(BLE_TOOLBOX_CONN_DISCONNECTED,
                                        (esp_err_t)event->connect.status, s_ble.user);
            }
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        s_ble.conn_state = BLE_TOOLBOX_CONN_DISCONNECTED;
        s_ble.pend_passkey = false;
        ESP_LOGI(TAG, "disconnected (reason %d)", event->disconnect.reason);
        if (s_ble.cbs.on_conn_state != NULL) {
            s_ble.cbs.on_conn_state(BLE_TOOLBOX_CONN_DISCONNECTED, ESP_OK, s_ble.user);
        }
        break;

    case BLE_GAP_EVENT_NOTIFY_RX:
        if (s_ble.cbs.on_conn_notify != NULL && event->notify_rx.om != NULL) {
            const uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
            uint8_t buf[64];
            const uint16_t n = (len < sizeof(buf)) ? len : (uint16_t)sizeof(buf);
            os_mbuf_copydata(event->notify_rx.om, 0, n, buf);
            s_ble.cbs.on_conn_notify(event->notify_rx.attr_handle, buf, n,
                                     event->notify_rx.indication, s_ble.user);
        }
        break;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        s_ble.pend_conn_handle = event->passkey.conn_handle;
        s_ble.pend_action = event->passkey.params.action;
        s_ble.pend_passkey = true;
        ESP_LOGI(TAG, "passkey action %u (numcmp %lu)",
                 (unsigned)event->passkey.params.action,
                 (unsigned long)event->passkey.params.numcmp);
        if (s_ble.cbs.on_passkey != NULL) {
            s_ble.cbs.on_passkey(event->passkey.conn_handle, event->passkey.params.action,
                                 event->passkey.params.numcmp, s_ble.user);
        }
        break;

    default:
        break;
    }

    return 0;
}

/* ---- NimBLE bring-up ----------------------------------------------------- */

static void tb_ble_on_sync(void)
{
    s_ble.ready = true;
    ESP_LOGI(TAG, "NimBLE synced with the co-processor's controller");
}

static void tb_ble_on_reset(int reason)
{
    /* Losing the controller invalidates a scan: the reports will simply stop, and
     * claiming otherwise would leave the UI showing a dead scan as live. */
    s_ble.ready = false;
    s_ble.scanning = false;
    ESP_LOGE(TAG, "controller reset, reason %d", reason);

    if (s_ble.cbs.on_scan_state != NULL) {
        s_ble.cbs.on_scan_state(false, ESP_FAIL, s_ble.user);
    }
}

static void tb_ble_host_task(void *arg)
{
    (void)arg;
    nimble_port_run();          /* returns only on nimble_port_stop() */
    nimble_port_freertos_deinit();
}

esp_err_t ble_toolbox_host_init(const ble_toolbox_host_callbacks_t *cbs, void *user)
{
    if (cbs != NULL) {
        s_ble.cbs = *cbs;
        s_ble.user = user;
    }

    if (s_ble.initialized) {
        return ESP_OK;          /* idempotent: reopening the app is cheap */
    }

    /* Bind the hosted HCI transport before NimBLE starts. This is the order the
     * shipped example uses, and it matters: nimble_port_init() immediately begins
     * talking to the controller through whatever transport the binding installed. */
    esp_hosted_bt_host_stack_cfg_t bt = ESP_HOSTED_BT_HOST_STACK_CONFIG_NIMBLE();

    /* The default is 5 seconds, after which bring_up_controller gives up and returns an
     * error. It is not returning an error here - so the co-processor is CLAIMING the
     * controller is up, and then not answering HCI on the byte pipe (every command times
     * out as BLE_HS_ETIMEOUT_HCI, with no 'controller init failed' to explain it).
     *
     * A longer window is set anyway, so that if the co-processor is simply not ready yet
     * when the app opens, this retries instead of accepting a premature yes. */
    bt.controller_ready_timeout_ms = 30000;

    esp_err_t err = esp_hosted_bt_host_stack_setup(&bt);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hosted BT bind failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb = tb_ble_on_sync;
    ble_hs_cfg.reset_cb = tb_ble_on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* Pairing: allow passkey entry and numeric comparison without forcing MITM, so
     * "just works" devices (most keyboards) still pair with no code. Bonding is
     * disabled: the default leaves bonding on while distributing no keys, which
     * leaves a peer expecting a stored long-term key that never existed and stalls
     * reconnection after a disconnect. */
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_KEYBOARD_DISPLAY;
    ble_hs_cfg.sm_bonding = 0;

    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    nimble_port_freertos_init(tb_ble_host_task);

    s_ble.initialized = true;

    if (s_scan_done == NULL) {
        s_scan_done = xSemaphoreCreateBinary();
    }

    /* Started BEFORE the sync is awaited, so it is running throughout. Its job is to resume
     * a scan the controller stops, and to be the thing that is still alive if this wait
     * times out — which is why it cannot live after the check that gives up. */
    if (xTaskCreate(tb_ble_scan_watchdog, "tb_ble_wd", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "no room for the scan watchdog; a stopped scan will stay stopped");
    }

    /* Wait for sync before declaring the module usable. Sync is reached only after
     * HCI Reset and the controller-information exchange have crossed the pipe, so it
     * is the honest definition of "the radio answered".
     *
     * A SHORT wait here, not a long one. Measured on this board the controller takes 30 to
     * 45 seconds from boot to answer, resetting several times on the way — one capture shows
     * "controller reset, reason 19" eight times at two second intervals before it settles,
     * and "NimBLE never synced" at 45.8 s. Waiting that long inside init() would block the
     * caller for most of a minute with nothing to show for it.
     *
     * So this covers only the common case where the radio is already up, and returns
     * ESP_ERR_TIMEOUT otherwise. That is not a verdict that the radio is dead: the watchdog
     * keeps waiting, and the caller is better placed to wait with it because it can report
     * progress and it knows a user is watching. */
    for (int i = 0; i < 100 && !s_ble.ready; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (!s_ble.ready) {
        ESP_LOGE(TAG, "NimBLE never synced: the co-processor's controller is not answering");
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "BLE host ready (NimBLE over the hosted HCI pipe)");
    return ESP_OK;
}

bool ble_toolbox_host_is_ready(void)
{
    return s_ble.ready;
}

esp_err_t ble_toolbox_host_wait_ready(uint32_t timeout_ms)
{
    if (s_ble.ready) {
        return ESP_OK;
    }

    /* Polled rather than event-driven: the flag is already written by the NimBLE host task (see
     * the sync callback), and adding a semaphore would mean a second thing to keep in step with it.
     * 50 ms is the granularity the rest of this module already waits at. */
    const uint32_t step_ms = 50;

    for (uint32_t waited = 0; waited < timeout_ms; waited += step_ms) {
        if (s_ble.ready) {
            ESP_LOGI(TAG, "controller answered after %u ms of waiting", (unsigned)waited);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(step_ms));
    }

    if (s_ble.ready) {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "controller still not answering after %u ms", (unsigned)timeout_ms);
    return ESP_ERR_TIMEOUT;
}

esp_err_t ble_toolbox_host_set_callbacks(const ble_toolbox_host_callbacks_t *cbs, void *user)
{
    if (cbs != NULL) {
        s_ble.cbs = *cbs;
        s_ble.user = user;
    }
    return ESP_OK;
}

esp_err_t ble_toolbox_host_begin(void)
{
    if (s_ble.ready) {
        return ESP_OK;
    }

    if (!s_ble.initialized) {
        /* init() blocks, but only for a few seconds, and only while the radio is cold —
         * which happens once. A caller that cannot afford even that can call this again
         * from wherever it is safe. */
        (void)ble_toolbox_host_init(NULL, NULL);
    }

    return ESP_ERR_NOT_FINISHED;
}

/* ---- Keeping a wanted scan running ---------------------------------------
 *
 * Measured: a scan asked for with duration 0 ends after about 10.2 seconds with
 * BLE_GAP_EVENT_DISC_COMPLETE — 747 advertisements and then silence. The host passes the
 * duration straight through to the HCI command, so the stop is the controller's decision
 * rather than a timeout in this code, and no amount of re-reading the host would change it.
 *
 * So the scan is resumed while it is still wanted. This also covers the controller
 * resetting under us, which happens: "controller reset, reason 19" appears in the logs and
 * takes the scan with it.
 *
 * The wait is why this is a task rather than a retry loop inside the event handler:
 * ble_gap_disc cannot be called from the NimBLE event context — it would deadlock against
 * the very callback it is running in. */
/* Recovery must never be able to block the watchdog, which is why it runs here.
 *
 * Measured: the previous inline version hung, once, and the watchdog died with it. The board
 * then sat for 88 minutes with a dead radio and nothing retrying, because the watchdog task
 * was blocked inside nimble_port_stop() waiting for a NimBLE host task that was itself
 * blocked in ble_hs_hci_cmd_tx waiting for an acknowledgement from a controller that was
 * never going to send one. The one task that could have kept trying was the one that got
 * stuck.
 *
 * The watchdog cannot fix a hang it is inside of, so the recovery is a separate task and the
 * watchdog waits for it with a deadline. A recovery that hangs is abandoned - leaked task and
 * all - rather than taking the retry loop down with it. Leaking a task in a case that
 * otherwise needs a power cycle is not a close call. */
static TaskHandle_t s_recovery_task;

/* When the in-flight attempt stops being worth waiting for. A recovery that hangs must not
 * block the next one forever: measured, one hang here meant the board never retried again. */
static TickType_t   s_recovery_deadline;
#define TB_BLE_RECOVERY_DEADLINE_MS  60000u

static void tb_ble_recovery_task(void *arg)
{
    (void)arg;

    ESP_LOGW(TAG, "recovering the host stack");

    /* nimble_port_stop() is NOT called, deliberately.
     *
     * It cannot succeed when the controller is dead, and that is not a guess - it is
     * NimBLE's own code:
     *
     *   ble_npl_sem_pend(&ble_hs_stop_sem, BLE_NPL_TIME_FOREVER);
     *
     * which waits for the host task to service a stop event, and that host task is blocked in
     * the controller that is not answering. A host blocked on a dead controller cannot
     * service its own stop event. Measured: the previous version hung here permanently -
     * logged "stopping the host" and then nothing for 525 seconds - and because a recovery
     * was only started when none was in flight, that one hang blocked every later attempt.
     * The board could never retry again.
     *
     * So this does the part that can work and skips the part that cannot: unbind and rebind.
     * Whether the stranded host task copes with a rebound transport is unproven, and if the
     * controller is truly dead nothing here will help anyway - but an attempt that might work
     * beats a call that provably cannot, and the deadline below bounds the damage if this
     * hangs too. */
    ESP_LOGW(TAG, "  recovery: unbinding the transport (skipping nimble_port_stop)");
    const int td_rc = esp_hosted_bt_host_stack_teardown();

    vTaskDelay(pdMS_TO_TICKS(500));

    /* Cleared so the bring-up below is a real bring-up. init() returns early when this is
     * set, which once made the "recovery" a teardown with no counterpart. */
    s_ble.initialized = false;
    s_ble.ready = false;
    s_ble.scanning = false;

    const esp_err_t rec = ble_toolbox_host_init(NULL, NULL);
    ESP_LOGW(TAG, "recovery done: teardown=%d init=%s -> ready=%d",
             td_rc, esp_err_to_name(rec), (int)s_ble.ready);
    s_recovery_task = NULL;
    vTaskDelete(NULL);
}

static void tb_ble_scan_watchdog(void *arg)
{
    (void)arg;

    int stuck_seconds = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!s_ble.initialized) {
            continue;
        }

        if (!s_ble.ready) {
            /* The controller has not answered. Waiting alone does not fix this: measured,
             * HCI commands time out ("controller reset, reason 19" - BLE_HS_ETIMEOUT) and
             * once NimBLE's initial sync has failed it does not retry by itself. */
            stuck_seconds++;

            if (stuck_seconds >= 5 && (stuck_seconds % 15) == 0) {
                /* RECOVERY IS NOT ATTEMPTED. This is a deliberate reversal, and the
                 * measurements behind it are worth keeping.
                 *
                 * Three versions were tried and all three made things worse:
                 *
                 *   v1  teardown then init()       - init() returns early when initialized is
                 *                                    set, so the transport was unbound every ten
                 *                                    seconds and never rebound. Crash.
                 *   v2  stop, teardown, init()     - nimble_port_stop() pends forever on a
                 *                                    semaphore the host task can only signal by
                 *                                    servicing a stop event, and that task is
                 *                                    blocked on the dead controller. The hang
                 *                                    took the watchdog with it; the board went
                 *                                    silent for 88 minutes.
                 *   v3  unbind and rebind, skipping - the transport was torn out from under a
                 *       the stop                       live NimBLE, which then failed EVERY HCI
                 *                                    command with rc=17 at ~10 per second:
                 *
                 *        864953  recovery: unbinding the transport (skipping nimble_port_stop)
                 *        865258  eh_host_feat_rpc: request: no response
                 *        866029  ble_hs_hci_cmd_transport rc=17   x303 and climbing
                 *
                 *                                    That flood is what the user saw as "it
                 *                                    seems to have hung": a permanently broken
                 *                                    transport shouting once per 100 ms.
                 *
                 * The conclusion is that there is no in-process recovery from a controller
                 * that has stopped answering, and trying costs more than doing nothing. A
                 * dead radio with a working UI is a far better failure than a live radio
                 * thread flooding the console.
                 *
                 * So the watchdog keeps its own clock, keeps reporting, and leaves the
                 * transport alone. Recovery needs a power cycle or a co-processor reflash,
                 * both of which are outside this process. */
                ESP_LOGW(TAG, "controller silent for %d s; no in-process recovery is possible "
                              "(needs a power cycle or a co-processor reflash)",
                         stuck_seconds);
            }
            continue;
        }

        stuck_seconds = 0;

        /* A scan is resumed, but NOT indefinitely.
         *
         * Measurements across several sessions put the controller's death at roughly 13
         * minutes of CONTINUOUS scanning - it is stopped by the controller every ~10 s and
         * this loop restarted it every time, so it never rested. Whether sustained load is
         * the cause or merely the trigger is not established, but the correlation is
         * consistent and the cost of the failure is a power cycle, so the load is capped.
         *
         * After the cap the scan is left stopped and says why. Re-pressing Start is a much
         * better outcome than a radio that needs unplugging, and the user can see the
         * difference: the list stops growing rather than the app going quiet. */
        if (s_ble.want_scan && !s_ble.scanning &&
            (s_ble.resume_at_ms == 0 ||
             (xTaskGetTickCount() * portTICK_PERIOD_MS) >= s_ble.resume_at_ms)) {
            if (s_ble.resume_count >= TB_BLE_MAX_RESUMES) {
                if (s_ble.resume_count == TB_BLE_MAX_RESUMES) {
                    s_ble.resume_count++;       /* once, not every second */
                    ESP_LOGW(TAG, "scan finished after %u resumes (~%u minutes); press Start "
                                  "again for another session",
                             (unsigned)TB_BLE_MAX_RESUMES,
                             (unsigned)((TB_BLE_MAX_RESUMES * 10u) / 60u));
                }
                continue;
            }

            /* A resume that FAILS backs off, and does not retry every second.
             *
             * Measured: with the controller up but refusing to accept a scan, this loop
             * retried once per second indefinitely - 61 identical failures in one capture,
             * each preceded by another failed HCI command. `resume_count` only advanced on
             * success, so the cap never engaged and nothing ever slowed it down. A retry
             * loop whose only exit is success has no exit.
             *
             * The backoff is exponential to a one-minute ceiling. The first retries are
             * quick because a transient refusal is common right after the controller starts;
             * later ones are slow because by then it is not transient. */
            const esp_err_t err = ble_toolbox_host_scan_start(&s_ble.params);
            if (err == ESP_OK) {
                s_ble.resume_count++;
                s_ble.resume_backoff_ms = 0;
                ESP_LOGW(TAG, "scan resumed after the controller stopped it (%u/%u)",
                         (unsigned)s_ble.resume_count, (unsigned)TB_BLE_MAX_RESUMES);
            } else {
                s_ble.resume_failures++;

                if (s_ble.resume_backoff_ms == 0) {
                    s_ble.resume_backoff_ms = 2000;
                } else if (s_ble.resume_backoff_ms < 60000) {
                    s_ble.resume_backoff_ms *= 2;
                    if (s_ble.resume_backoff_ms > 60000) {
                        s_ble.resume_backoff_ms = 60000;
                    }
                }

                /* Reported once per backoff step rather than on every attempt: the point of
                 * the backoff is to stop filling the console with one repeated line. */
                ESP_LOGW(TAG, "scan resume failed (%u so far, %s); next attempt in %u s",
                         (unsigned)s_ble.resume_failures, esp_err_to_name(err),
                         (unsigned)(s_ble.resume_backoff_ms / 1000));

                s_ble.resume_at_ms = xTaskGetTickCount() * portTICK_PERIOD_MS
                                     + s_ble.resume_backoff_ms;
            }
        }
    }
}

/* ---- Scanning ------------------------------------------------------------ */

esp_err_t ble_toolbox_host_scan_start(const ble_toolbox_scan_params_t *params)
{
    if (!s_ble.initialized) {
        const esp_err_t err = ble_toolbox_host_init(NULL, NULL);
        if (err != ESP_OK) {
            return err;
        }
    }

    if (!s_ble.ready) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_ble.scanning) {
        return ESP_ERR_INVALID_STATE;
    }

    s_ble.params = (params != NULL) ? *params : (ble_toolbox_scan_params_t){ 0 };

    struct ble_gap_disc_params dp = { 0 };
    dp.passive = s_ble.params.passive ? 1 : 0;
    dp.filter_duplicates = s_ble.params.filter_duplicates ? 1 : 0;
    dp.filter_policy = 0;
    dp.limited = 0;

    /* The controller takes interval and window in 0.625 ms units, and the window must
     * not exceed the interval or the controller rejects the command. Clamping rather
     * than failing: a scan that is slightly denser than asked for is better than a
     * scan that refuses to start over an arithmetic detail the caller cannot see. */
    const uint16_t interval_ms = (s_ble.params.interval_ms != 0)
                                 ? s_ble.params.interval_ms : TB_BLE_SCAN_INTERVAL_MS;
    uint16_t window_ms = (s_ble.params.window_ms != 0)
                         ? s_ble.params.window_ms : TB_BLE_SCAN_WINDOW_MS;
    if (window_ms > interval_ms) {
        window_ms = interval_ms;
    }

    /* Rounded up, because 0 units would mean "no scan window" rather than "a short
     * one". */
    dp.itvl = (uint16_t)((interval_ms * 8 + 4) / 5);
    dp.window = (uint16_t)((window_ms * 8 + 4) / 5);
    if (dp.window == 0) {
        dp.window = 1;
    }

    s_ble.adv_count = 0;
    s_ble.resume_count = 0;
    s_ble.resume_failures = 0;
    s_ble.resume_backoff_ms = 0;
    s_ble.resume_at_ms = 0;

    /* want_scan is set AFTER the start succeeds, not before.
     *
     * Setting it first meant a start that FAILED still left the flag set, so the resume loop
     * treated a scan that never began as one that had stopped and kept restarting it. Combined
     * with the caller's 500 ms UI timer retrying as well, a controller refusing the command got
     * hammered from two directions at once:
     *
     *     E (442248) tb_ble: ble_gap_disc failed: rc=17
     *     E (443280) tb_ble: ble_gap_disc failed: rc=17
     *     ... every 1.032 s, indefinitely
     *
     * rc=17 is BLE_HS_EBUSY, the same code the withdrawn unbind/rebind recovery produced ten times
     * a second, and the load it represents is the leading suspect for the co-processor faults this
     * project has spent so long on. A retry storm is not a neutral response to a busy controller;
     * it is what keeps it busy.
     *
     * On failure the flag stays clear, so the resume loop leaves it alone. Re-pressing Start is
     * then the only thing that tries again, which is the right shape: the user can see the failure
     * on screen, and one deliberate attempt is not a storm. */
    const int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, TB_BLE_SCAN_DURATION_MS, &dp,
                                tb_ble_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc failed: rc=%d", rc);

        /* Reported as the state it is, so the UI can say something a person can act on rather than
         * the bare name of an error code. */
        return (rc == BLE_HS_EBUSY) ? ESP_ERR_INVALID_STATE : ESP_FAIL;
    }

    s_ble.want_scan = true;
    s_ble.scanning = true;
    ESP_LOGI(TAG, "scan started (interval %u ms, window %u ms, %s, dup-filter %s)",
             (unsigned)interval_ms, (unsigned)window_ms,
             dp.passive ? "passive" : "active",
             dp.filter_duplicates ? "on" : "off");

    if (s_ble.cbs.on_scan_state != NULL) {
        s_ble.cbs.on_scan_state(true, ESP_OK, s_ble.user);
    }

    return ESP_OK;
}

esp_err_t ble_toolbox_host_scan_stop(void)
{
    /* Cleared first, and regardless of whether a scan is running: this is the user saying
     * stop, and the watcher below must not restart it a moment later. */
    s_ble.want_scan = false;

    if (!s_ble.scanning) {
        return ESP_OK;          /* stopping a stopped scan is not an error */
    }

    const int rc = ble_gap_disc_cancel();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ble_gap_disc_cancel rc=%d", rc);
    }

    s_ble.scanning = false;

    if (s_ble.cbs.on_scan_state != NULL) {
        s_ble.cbs.on_scan_state(false, ESP_OK, s_ble.user);
    }

    return ESP_OK;
}

esp_err_t ble_toolbox_host_scan_stop_no_cancel(void)
{
    /* Stop the watcher only. The scan itself is left running until the controller ends
     * it on its own: issuing ble_gap_disc_cancel() (scan-disable) hangs the C6. */
    s_ble.want_scan = false;
    return ESP_OK;
}

bool ble_toolbox_host_is_scanning(void)
{
    return s_ble.scanning;
}

uint32_t ble_toolbox_host_adv_count(void)
{
    return s_ble.adv_count;
}

/* ---- Connection (GATT client) ------------------------------------------- */

/* Format a NimBLE UUID into its text form ("0x1800" for the standard 16-bit ones,
 * the full string for 128-bit). */
static void tb_ble_format_uuid(const ble_uuid_t *uuid, char *out, size_t out_len)
{
    char tmp[BLE_UUID_STR_LEN];
    ble_uuid_to_str(uuid, tmp);
    snprintf(out, out_len, "%s", tmp);
}

/* Service discovery: one callback per service, then a final EDONE with service == NULL. */
static int tb_ble_disc_svc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                              const struct ble_gatt_svc *service, void *arg)
{
    (void)conn_handle; (void)arg;

    if (s_ble.cbs.on_conn_svc == NULL) {
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        s_ble.cbs.on_conn_svc(NULL, s_ble.user);
        return 0;
    }
    if (error->status != 0 || service == NULL) {
        return 0;
    }

    ble_toolbox_gatt_svc_t svc;
    memset(&svc, 0, sizeof(svc));
    svc.start_handle = service->start_handle;
    svc.end_handle = service->end_handle;
    tb_ble_format_uuid(&service->uuid.u, svc.uuid_str, sizeof(svc.uuid_str));
    s_ble.cbs.on_conn_svc(&svc, s_ble.user);
    return 0;
}

/* Characteristic discovery: one callback per characteristic, then a final EDONE. */
static int tb_ble_disc_chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                              const struct ble_gatt_chr *chr, void *arg)
{
    (void)conn_handle; (void)arg;

    if (s_ble.cbs.on_conn_chr == NULL) {
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        s_ble.cbs.on_conn_chr(NULL, s_ble.user);
        return 0;
    }
    if (error->status != 0 || chr == NULL) {
        return 0;
    }

    ble_toolbox_gatt_chr_t c;
    memset(&c, 0, sizeof(c));
    c.val_handle = chr->val_handle;
    c.properties = chr->properties;
    tb_ble_format_uuid(&chr->uuid.u, c.uuid_str, sizeof(c.uuid_str));
    s_ble.cbs.on_conn_chr(&c, s_ble.user);
    return 0;
}

/* Read: copy the mbuf out before NimBLE frees it. */
static int tb_ble_read_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle; (void)arg;

    if (s_ble.cbs.on_conn_read == NULL) {
        return 0;
    }

    const uint16_t handle = (attr != NULL) ? attr->handle : 0;
    if (error->status != 0 || attr == NULL || attr->om == NULL) {
        s_ble.cbs.on_conn_read(handle, NULL, 0, s_ble.user);
        return 0;
    }

    uint16_t len = OS_MBUF_PKTLEN(attr->om);
    uint8_t buf[128];
    if (len > sizeof(buf)) {
        len = sizeof(buf);
    }
    os_mbuf_copydata(attr->om, 0, len, buf);
    s_ble.cbs.on_conn_read(handle, buf, len, s_ble.user);
    return 0;
}

esp_err_t ble_toolbox_host_connect(const uint8_t addr[6], bool random_addr)
{
    if (!s_ble.ready) {
        return ESP_ERR_INVALID_STATE;
    }

    ble_addr_t peer = { 0 };
    peer.type = random_addr ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC;
    /* ble_addr_t.val is the controller's byte order (least-significant first), which
     * is what the scan reported in `addr`. */
    memcpy(peer.val, addr, 6);

    /* Retry loop: wait out the scan's auto-stop (blocking on the semaphore), then connect.
     * Each attempt re-waits for a clean auto-stop before connecting. */
    for (int attempt = 0; attempt < 3; attempt++) {
        if (s_ble.scanning) {
            s_ble.want_scan = true;
            if (s_scan_done != NULL &&
                xSemaphoreTake(s_scan_done, pdMS_TO_TICKS(10000)) != pdTRUE) {
                ESP_LOGW(TAG, "connect: scan auto-stop timed out (attempt %d)", attempt);
                continue;
            }
        }

        /* Stop the watcher now so the scan does not restart under the connect. */
        (void)ble_toolbox_host_scan_stop_no_cancel();
        if (s_ble.conn_state != BLE_TOOLBOX_CONN_DISCONNECTED) {
            return ESP_ERR_INVALID_STATE;
        }

        const int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &peer, 10000, NULL,
                                       tb_ble_gap_event, NULL);
        if (rc == 0) {
            s_ble.conn_state = BLE_TOOLBOX_CONN_CONNECTING;
            if (s_ble.cbs.on_conn_state != NULL) {
                s_ble.cbs.on_conn_state(BLE_TOOLBOX_CONN_CONNECTING, ESP_OK, s_ble.user);
            }
            return ESP_OK;
        }
        ESP_LOGW(TAG, "connect: ble_gap_connect rc=%d (attempt %d)", rc, attempt);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    return ESP_FAIL;
}

esp_err_t ble_toolbox_host_disconnect(void)
{
    if (s_ble.conn_state == BLE_TOOLBOX_CONN_DISCONNECTED) {
        return ESP_OK;
    }

    /* The state transition to DISCONNECTED is reported by the BLE_GAP_EVENT_DISCONNECT
     * handler, not here, so it stays a single source. */
    const int rc = ble_gap_terminate(s_ble.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    if (rc != 0 && rc != BLE_HS_ENOTCONN) {
        ESP_LOGW(TAG, "ble_gap_terminate rc=%d", rc);
    }
    return ESP_OK;
}

esp_err_t ble_toolbox_host_discover_services(void)
{
    if (s_ble.conn_state != BLE_TOOLBOX_CONN_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }

    const int rc = ble_gattc_disc_all_svcs(s_ble.conn_handle, tb_ble_disc_svc_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gattc_disc_all_svcs failed: rc=%d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t ble_toolbox_host_discover_chars(uint16_t start_handle, uint16_t end_handle)
{
    if (s_ble.conn_state != BLE_TOOLBOX_CONN_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }

    const int rc = ble_gattc_disc_all_chrs(s_ble.conn_handle, start_handle, end_handle,
                                           tb_ble_disc_chr_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gattc_disc_all_chrs failed: rc=%d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t ble_toolbox_host_read(uint16_t val_handle)
{
    if (s_ble.conn_state != BLE_TOOLBOX_CONN_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }

    const int rc = ble_gattc_read(s_ble.conn_handle, val_handle, tb_ble_read_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gattc_read failed: rc=%d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t ble_toolbox_host_write(uint16_t val_handle, const uint8_t *data, uint16_t len)
{
    if (s_ble.conn_state != BLE_TOOLBOX_CONN_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }

    const int rc = ble_gattc_write_flat(s_ble.conn_handle, val_handle, data, len,
                                        tb_ble_read_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gattc_write_flat failed: rc=%d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t ble_toolbox_host_subscribe(uint16_t val_handle, bool want_indication)
{
    if (s_ble.conn_state != BLE_TOOLBOX_CONN_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }

    /* The Client Characteristic Configuration Descriptor (CCCD) is the descriptor
     * immediately after the value handle. 0x0001 enables notifications, 0x0002
     * enables indications. */
    const uint16_t cccd_handle = val_handle + 1;
    const uint16_t value = want_indication ? 0x0002 : 0x0001;
    const uint8_t data[2] = { (uint8_t)(value & 0xff), (uint8_t)(value >> 8) };

    const int rc = ble_gattc_write_flat(s_ble.conn_handle, cccd_handle, data, 2,
                                        tb_ble_read_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "CCCD write (0x%04x) failed: rc=%d", (unsigned)cccd_handle, rc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "subscribed on 0x%04x (%s)", (unsigned)val_handle,
             want_indication ? "indications" : "notifications");
    return ESP_OK;
}

ble_toolbox_conn_state_t ble_toolbox_host_conn_state(void)
{
    return s_ble.conn_state;
}

esp_err_t ble_toolbox_host_passkey_reply(uint32_t passkey, bool accept)
{
    if (!s_ble.pend_passkey) {
        return ESP_ERR_INVALID_STATE;
    }

    struct ble_sm_io pkey = { 0 };
    pkey.action = s_ble.pend_action;

    if (s_ble.pend_action == BLE_SM_IOACT_NUMCMP) {
        pkey.numcmp_accept = accept ? 1 : 0;
    } else {
        pkey.passkey = passkey;
    }

    const uint16_t conn_handle = s_ble.pend_conn_handle;
    s_ble.pend_passkey = false;

    const int rc = ble_sm_inject_io(conn_handle, &pkey);
    if (rc != 0) {
        ESP_LOGE(TAG, "passkey inject failed: %d", rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "passkey answered (action %u, passkey %lu)",
             (unsigned)s_ble.pend_action, (unsigned long)passkey);
    return ESP_OK;
}











