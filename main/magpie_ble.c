#include "magpie_ble.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *const TAG = "magpie_ble";
static const char DEVICE_NAME[] = "FoloToy AI Passport";
static const uint32_t PAIRING_WINDOW_MS = 60000;

/* UUID D1749A01/02/03-65B4-46F7-A6BA-DA9D2AB61750 in NimBLE byte order. */
#define MAGPIE_UUID(characteristic) BLE_UUID128_INIT( \
    0x50, 0x17, 0xB6, 0x2A, 0x9D, 0xDA, 0xBA, 0xA6, \
    0xF7, 0x46, 0xB4, 0x65, characteristic, 0x9A, 0x74, 0xD1)

static const ble_uuid128_t service_uuid = MAGPIE_UUID(0x01);
static const ble_uuid128_t rx_uuid = MAGPIE_UUID(0x02);
static const ble_uuid128_t tx_uuid = MAGPIE_UUID(0x03);

/* Only the NimBLE host task mutates the transport and model after initialization. */
static QueueHandle_t view_queue;
static ble_store_write_fn *write_bond_store;
static struct ble_npl_event pairing_request;
static struct ble_npl_callout pairing_timer;
static struct ble_npl_callout advertising_retry;
static magpie_ble_view_t view;
static magpie_stream_t input;
static bool host_synced;
static bool subscribed;
static bool secure;
static bool fresh_pairing;
static bool stored_our_key;
static bool stored_peer_key;
static uint16_t connection = BLE_HS_CONN_HANDLE_NONE;
static uint16_t tx_handle;
static uint8_t own_address_type;

/* ESP-IDF's NimBLE port exports the bond-store initializer without a public declaration. */
void ble_store_config_init(void);

static int on_gap_event(struct ble_gap_event *event, void *argument);

static void publish_view(void)
{
    view.ready = secure && subscribed;
    xQueueOverwrite(view_queue, &view);
}

/* NimBLE persists candidate keys before sending ENC_CHANGE. Restrict writes
 * here so a late or unauthenticated pairing cannot replace the existing Mac. A
 * bonded reconnect reads the keys, leaving it free to use a rotating address. */
static int store_write(int object_type, const union ble_store_value *value)
{
    bool security_record = object_type == BLE_STORE_OBJ_TYPE_OUR_SEC ||
                           object_type == BLE_STORE_OBJ_TYPE_PEER_SEC;
    if (security_record && (!view.pairing || !value->sec.authenticated || !value->sec.sc ||
                            !value->sec.ltk_present || value->sec.key_size != 16)) {
        return BLE_HS_EREJECT;
    }
    int result = write_bond_store(object_type, value);
    if (result == 0) {
        stored_our_key |= object_type == BLE_STORE_OBJ_TYPE_OUR_SEC;
        stored_peer_key |= object_type == BLE_STORE_OBJ_TYPE_PEER_SEC;
    }
    return result;
}

static int store_status(struct ble_store_status_event *event, void *argument)
{
    if (event->event_code == BLE_STORE_EVENT_OVERFLOW && !view.pairing) {
        return BLE_HS_ESTORE_CAP;
    }
    return ble_store_util_status_rr(event, argument);
}

static void load_bond_state(void)
{
    ble_addr_t peer;
    int peers = 0;
    int result = ble_store_util_bonded_peers(&peer, &peers, 1);
    if (result == 0) {
        view.has_bond = peers > 0;
    }
}

static void reconcile_advertising(void)
{
    if (!host_synced) {
        return;
    }
    bool wanted = connection == BLE_HS_CONN_HANDLE_NONE && (view.pairing || view.has_bond);
    if (!wanted) {
        ble_npl_callout_stop(&advertising_retry);
        if (ble_gap_adv_active()) {
            ble_gap_adv_stop();
        }
        return;
    }
    if (ble_gap_adv_active()) {
        return;
    }

    const struct ble_hs_adv_fields fields = {
        .flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP,
        .uuids128 = &service_uuid,
        .num_uuids128 = 1,
        .uuids128_is_complete = 1,
    };
    const struct ble_hs_adv_fields response = {
        .name = (const uint8_t *)DEVICE_NAME,
        .name_len = sizeof(DEVICE_NAME) - 1,
        .name_is_complete = 1,
    };
    int result = ble_gap_adv_set_fields(&fields);
    if (result == 0) {
        result = ble_gap_adv_rsp_set_fields(&response);
    }
    if (result == 0) {
        struct ble_gap_adv_params parameters = {
            .conn_mode = BLE_GAP_CONN_MODE_UND,
            .disc_mode = BLE_GAP_DISC_MODE_GEN,
            /* First pairing is responsive; bonded idle advertises four times per second. */
            .itvl_min = view.pairing ? 96 : 384,
            .itvl_max = view.pairing ? 128 : 416,
        };
        result = ble_gap_adv_start(own_address_type, NULL, BLE_HS_FOREVER,
                                   &parameters, on_gap_event, NULL);
    }
    if (result != 0) {
        ESP_LOGW(TAG, "Advertising retry (%d)", result);
        ble_npl_callout_reset(&advertising_retry, ble_npl_time_ms_to_ticks32(1000));
    }
}

static void on_advertising_retry(struct ble_npl_event *event)
{
    (void)event;
    reconcile_advertising();
}

static void clear_connection(void)
{
    connection = BLE_HS_CONN_HANDLE_NONE;
    secure = false;
    subscribed = false;
    fresh_pairing = false;
    stored_our_key = false;
    stored_peer_key = false;
    view.showing_passkey = false;
    view.passkey = 0;
    magpie_stream_reset(&input);
}

static void on_pairing_timeout(struct ble_npl_event *event)
{
    (void)event;
    view.pairing = false;
    view.showing_passkey = false;
    view.passkey = 0;
    if (connection != BLE_HS_CONN_HANDLE_NONE && !secure) {
        ble_gap_terminate(connection, BLE_ERR_REM_USER_CONN_TERM);
    }
    publish_view();
    reconcile_advertising();
}

static void on_pairing_request(struct ble_npl_event *event)
{
    (void)event;
    if (secure) {
        return;
    }
    if (ble_npl_callout_reset(&pairing_timer, ble_npl_time_ms_to_ticks32(PAIRING_WINDOW_MS)) != BLE_NPL_OK) {
        ESP_LOGW(TAG, "Cannot start pairing timer");
        return;
    }
    view.pairing = true;
    publish_view();
    reconcile_advertising();
}

static bool is_secure(uint16_t handle)
{
    struct ble_gap_conn_desc description;
    return ble_gap_conn_find(handle, &description) == 0 &&
           description.sec_state.encrypted && description.sec_state.authenticated &&
           description.sec_state.bonded && description.sec_state.key_size == 16;
}

static uint32_t new_passkey(void)
{
    const uint32_t limit = UINT32_MAX - (UINT32_MAX % 1000000U);
    uint32_t value;
    do {
        value = esp_random();
    } while (value >= limit);
    return value % 1000000U;
}

static int notify_reply(uint16_t handle, magpie_reply_t reply)
{
    const char *text = reply == MAGPIE_REPLY_PONG ? "MPU1,PONG\n" : "MPU1,ACK\n";
    struct os_mbuf *packet = ble_hs_mbuf_from_flat(text, strlen(text));
    if (packet == NULL) {
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return ble_gatts_notify_custom(handle, tx_handle, packet) == 0 ? 0 : BLE_ATT_ERR_UNLIKELY;
}

static int on_gatt_access(uint16_t handle, uint16_t attribute,
                          struct ble_gatt_access_ctxt *context, void *argument)
{
    (void)attribute;
    (void)argument;
    if (context->op != BLE_GATT_ACCESS_OP_WRITE_CHR ||
        ble_uuid_cmp(context->chr->uuid, &rx_uuid.u) != 0) {
        return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    }
    if (handle != connection || !secure || !is_secure(handle)) {
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }
    if (!subscribed) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    uint8_t bytes[MAGPIE_FRAME_CAPACITY];
    uint16_t length = OS_MBUF_PKTLEN(context->om);
    if (length == 0 || length > sizeof(bytes) ||
        os_mbuf_copydata(context->om, 0, length, bytes) != 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000;
    for (uint16_t i = 0; i < length; ++i) {
        magpie_reply_t reply = magpie_stream_receive(&input, &view.link, bytes[i], now_ms);
        if (reply == MAGPIE_REPLY_ACK) {
            publish_view();
        }
        if (reply != MAGPIE_REPLY_NONE) {
            int result = notify_reply(handle, reply);
            if (result != 0) {
                return result;
            }
        }
    }
    return 0;
}

static const struct ble_gatt_svc_def services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &rx_uuid.u,
                .access_cb = on_gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN,
                .min_key_size = 16,
            },
            {
                .uuid = &tx_uuid.u,
                .access_cb = on_gatt_access,
                .val_handle = &tx_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC |
                         BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN,
            },
            {0},
        },
    },
    {0},
};

static int on_gap_event(struct ble_gap_event *event, void *argument)
{
    (void)argument;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            reconcile_advertising();
            return 0;
        }
        connection = event->connect.conn_handle;
        {
            int result = ble_gap_security_initiate(connection);
            if (result != 0 && result != BLE_HS_EALREADY) {
                ble_gap_terminate(connection, BLE_ERR_REM_USER_CONN_TERM);
            }
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        clear_connection();
        load_bond_state();
        publish_view();
        reconcile_advertising();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        reconcile_advertising();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.conn_handle != connection) {
            return 0;
        }
        /* Check the actual link even when central and peripheral encryption requests race.
         * For a new ceremony, a negotiated bond alone does not imply NVS writes succeeded. */
        secure = is_secure(connection) &&
                 (!fresh_pairing || (view.pairing && stored_our_key && stored_peer_key));
        if (secure) {
            fresh_pairing = false;
            view.pairing = false;
            ble_npl_callout_stop(&pairing_timer);
            load_bond_state();
        } else {
            ble_gap_terminate(connection, BLE_ERR_REM_USER_CONN_TERM);
        }
        view.showing_passkey = false;
        view.passkey = 0;
        publish_view();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.conn_handle == connection && event->subscribe.attr_handle == tx_handle) {
            subscribed = event->subscribe.cur_notify != 0;
            publish_view();
        }
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (event->passkey.conn_handle == connection && view.pairing &&
            event->passkey.params.action == BLE_SM_IOACT_DISP) {
            struct ble_sm_io io = {
                .action = BLE_SM_IOACT_DISP,
                .passkey = new_passkey(),
            };
            fresh_pairing = true;
            stored_our_key = false;
            stored_peer_key = false;
            view.showing_passkey = true;
            view.passkey = io.passkey;
            publish_view();
            int result = ble_sm_inject_io(connection, &io);
            if (result != 0) {
                view.showing_passkey = false;
                view.passkey = 0;
                publish_view();
                ble_gap_terminate(connection, BLE_ERR_REM_USER_CONN_TERM);
            }
            return result;
        }
        ble_gap_terminate(event->passkey.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc description;
        if (view.pairing && event->repeat_pairing.conn_handle == connection &&
            ble_gap_conn_find(connection, &description) == 0 &&
            ble_store_util_delete_peer(&description.peer_id_addr) == 0) {
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }
        ble_gap_terminate(event->repeat_pairing.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    }

    default:
        return 0;
    }
}

static void on_host_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset (%d)", reason);
    host_synced = false;
    clear_connection();
    publish_view();
}

static void on_host_sync(void)
{
    int result = ble_hs_util_ensure_addr(0);
    if (result == 0) {
        result = ble_hs_id_infer_auto(0, &own_address_type);
    }
    if (result != 0) {
        ESP_LOGE(TAG, "No BLE address (%d)", result);
        return;
    }
    host_synced = true;
    load_bond_state();
    publish_view();
    reconcile_advertising();
}

static void host_task(void *argument)
{
    (void)argument;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t magpie_ble_init(QueueHandle_t view_updates)
{
    view_queue = view_updates;
    esp_err_t error = nimble_port_init();
    if (error != ESP_OK) {
        return error;
    }

    ble_hs_cfg.reset_cb = on_host_reset;
    ble_hs_cfg.sync_cb = on_host_sync;
    ble_hs_cfg.store_status_cb = store_status;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_sc_only = 1;
    ble_hs_cfg.sm_sec_lvl = 4;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int result = ble_svc_gap_device_name_set(DEVICE_NAME);
    if (result == 0) {
        result = ble_gatts_count_cfg(services);
    }
    if (result == 0) {
        result = ble_gatts_add_svcs(services);
    }
    if (result != 0) {
        ESP_LOGE(TAG, "Cannot register GATT service (%d)", result);
        return ESP_FAIL;
    }
    ble_store_config_init();
    write_bond_store = ble_hs_cfg.store_write_cb;
    if (write_bond_store == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ble_hs_cfg.store_write_cb = store_write;

    struct ble_npl_eventq *events = nimble_port_get_dflt_eventq();
    ble_npl_event_init(&pairing_request, on_pairing_request, NULL);
    if (ble_npl_callout_init(&pairing_timer, events, on_pairing_timeout, NULL) != 0 ||
        ble_npl_callout_init(&advertising_retry, events, on_advertising_retry, NULL) != 0) {
        return ESP_ERR_NO_MEM;
    }
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

void magpie_ble_request_pairing(void)
{
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &pairing_request);
}
