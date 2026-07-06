#include "ble_tpms.h"
#include "ble_srv_common.h"
#include "app_error.h"
#include <string.h>

static ble_uuid128_t m_tpms_uuid_base = { .uuid128 = BLE_UUID_TPMS_SERVICE_BASE };

static void on_write(ble_tpms_t *p_tpms, ble_evt_t const *p_ble_evt)
{
    ble_gatts_evt_write_t const *p = &p_ble_evt->evt.gatts_evt.params.write;
    if (p->handle == p_tpms->command_char_handles.value_handle && p->len >= 1 && p_tpms->write_handler) {
        uint8_t cmd = p->data[0];
        const uint8_t *data = (p->len > 1) ? &p->data[1] : NULL;
        p_tpms->write_handler(p_tpms, cmd, data, p->len - 1);
    }
    if (p->handle == p_tpms->config_char_handles.value_handle && p->len >= sizeof(ble_tpms_config_t) && p_tpms->write_handler) {
        p_tpms->write_handler(p_tpms, TPMS_CMD_SET_CONFIG, p->data, p->len);
    }
}

void ble_tpms_on_ble_evt(ble_tpms_t *p_tpms, ble_evt_t const *p_ble_evt)
{
    if (!p_tpms || !p_ble_evt) return;
    switch (p_ble_evt->header.evt_id) {
        case BLE_GAP_EVT_CONNECTED:
            p_tpms->conn_handle = p_ble_evt->evt.gap_evt.conn_handle; break;
        case BLE_GAP_EVT_DISCONNECTED:
            p_tpms->conn_handle = BLE_CONN_HANDLE_INVALID; break;
        case BLE_GATTS_EVT_WRITE:
            on_write(p_tpms, p_ble_evt); break;
    }
}

static uint32_t add_char(ble_tpms_t *p_tpms, uint16_t uuid, ble_gatts_char_md_t *char_md,
                          ble_gatts_attr_md_t *attr_md, uint16_t max_len, ble_gatts_char_handles_t *handles)
{
    ble_uuid_t ble_uuid = {.type = p_tpms->uuid_type, .uuid = uuid};
    ble_gatts_attr_t attr = {
        .p_uuid = &ble_uuid, .p_attr_md = attr_md,
        .init_len = 0, .max_len = max_len, .p_value = NULL
    };
    return sd_ble_gatts_characteristic_add(p_tpms->service_handle, char_md, &attr, handles);
}

uint32_t ble_tpms_init(ble_tpms_t *p_tpms, ble_tpms_write_handler_t write_handler)
{
    uint32_t err;
    ble_uuid_t ble_uuid;
    if (!p_tpms) return NRF_ERROR_NULL;

    p_tpms->conn_handle = BLE_CONN_HANDLE_INVALID;
    p_tpms->write_handler = write_handler;

    err = sd_ble_uuid_vs_add(&m_tpms_uuid_base, &p_tpms->uuid_type);
    if (err != NRF_SUCCESS) return err;

    ble_uuid = (ble_uuid_t){.type = p_tpms->uuid_type, .uuid = BLE_UUID_TPMS_SERVICE};
    err = sd_ble_gatts_service_add(BLE_GATTS_SRVC_TYPE_PRIMARY, &ble_uuid, &p_tpms->service_handle);
    if (err != NRF_SUCCESS) return err;

    // Sensor (Notify)
    ble_gatts_char_md_t cm = {.char_props = {.read = 1, .notify = 1}};
    ble_gatts_attr_md_t am = {0};
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&am.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(&am.write_perm);
    am.vloc = BLE_GATTS_VLOC_STACK; am.vlen = 1;
    err = add_char(p_tpms, BLE_UUID_TPMS_SENSOR_CHAR, &cm, &am, 20, &p_tpms->sensor_char_handles);
    if (err != NRF_SUCCESS) return err;

    // Config (Read/Write)
    cm = (ble_gatts_char_md_t){.char_props = {.read = 1, .write = 1}};
    am = (ble_gatts_attr_md_t){0};
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&am.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&am.write_perm);
    am.vloc = BLE_GATTS_VLOC_STACK;
    err = add_char(p_tpms, BLE_UUID_TPMS_CONFIG_CHAR, &cm, &am, sizeof(ble_tpms_config_t), &p_tpms->config_char_handles);
    if (err != NRF_SUCCESS) return err;

    // Command (Write)
    cm = (ble_gatts_char_md_t){.char_props = {.write = 1, .write_wo_resp = 1}};
    am = (ble_gatts_attr_md_t){0};
    BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(&am.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&am.write_perm);
    am.vloc = BLE_GATTS_VLOC_STACK; am.vlen = 1;
    err = add_char(p_tpms, BLE_UUID_TPMS_COMMAND_CHAR, &cm, &am, 20, &p_tpms->command_char_handles);
    if (err != NRF_SUCCESS) return err;

    // Status (Read/Notify)
    cm = (ble_gatts_char_md_t){.char_props = {.read = 1, .notify = 1}};
    am = (ble_gatts_attr_md_t){0};
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&am.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(&am.write_perm);
    am.vloc = BLE_GATTS_VLOC_STACK; am.vlen = 1;
    return add_char(p_tpms, BLE_UUID_TPMS_STATUS_CHAR, &cm, &am, 20, &p_tpms->status_char_handles);
}

uint32_t ble_tpms_sensor_notify(ble_tpms_t *p_tpms, const ble_tpms_sensor_t *p_sensor)
{
    if (!p_tpms || !p_sensor || p_tpms->conn_handle == BLE_CONN_HANDLE_INVALID) return NRF_ERROR_INVALID_STATE;
    uint8_t buf[11];
    buf[0] = (p_sensor->sensor_id >> 24) & 0xFF;
    buf[1] = (p_sensor->sensor_id >> 16) & 0xFF;
    buf[2] = (p_sensor->sensor_id >> 8) & 0xFF;
    buf[3] = p_sensor->sensor_id & 0xFF;
    buf[4] = p_sensor->pressure_kpa;
    buf[5] = (uint8_t)p_sensor->temperature_c;
    buf[6] = p_sensor->battery_ok;
    buf[7] = p_sensor->count;
    buf[8] = (uint8_t)(p_sensor->rssi + 100);
    buf[9] = p_sensor->valid ? 1 : 0;
    uint16_t len = 10;
    ble_gatts_hvx_params_t hvx = {.handle = p_tpms->sensor_char_handles.value_handle,
                                   .type = BLE_GATT_HVX_NOTIFICATION, .p_len = &len, .p_data = buf};
    return sd_ble_gatts_hvx(p_tpms->conn_handle, &hvx);
}

uint32_t ble_tpms_status_update(ble_tpms_t *p_tpms, uint8_t status)
{
    if (!p_tpms || p_tpms->conn_handle == BLE_CONN_HANDLE_INVALID) return NRF_ERROR_INVALID_STATE;
    ble_gatts_value_t v = {.len = 1, .p_value = &status};
    return sd_ble_gatts_value_set(p_tpms->conn_handle, p_tpms->status_char_handles.value_handle, &v);
}

uint32_t ble_tpms_config_update(ble_tpms_t *p_tpms, const ble_tpms_config_t *p_config)
{
    if (!p_tpms || !p_config || p_tpms->conn_handle == BLE_CONN_HANDLE_INVALID) return NRF_ERROR_INVALID_STATE;
    ble_gatts_value_t v = {.len = sizeof(ble_tpms_config_t), .p_value = (uint8_t*)p_config};
    return sd_ble_gatts_value_set(p_tpms->conn_handle, p_tpms->config_char_handles.value_handle, &v);
}
