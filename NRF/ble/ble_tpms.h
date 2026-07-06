#ifndef BLE_TPMS_H
#define BLE_TPMS_H

#include <stdint.h>
#include <stdbool.h>
#include "ble.h"
#include "ble_srv_common.h"

#define BLE_UUID_TPMS_SERVICE_BASE {0xF0,0xDE,0xBC,0x9A,0x78,0x56,0x34,0x12,0x78,0x56,0x34,0x12,0,0,0,0}
#define BLE_UUID_TPMS_SERVICE      0x1234
#define BLE_UUID_TPMS_SENSOR_CHAR  0x1235
#define BLE_UUID_TPMS_CONFIG_CHAR  0x1236
#define BLE_UUID_TPMS_COMMAND_CHAR 0x1237
#define BLE_UUID_TPMS_STATUS_CHAR  0x1238

#define TPMS_CMD_START_SNIFFER  0x01
#define TPMS_CMD_STOP_SNIFFER   0x02
#define TPMS_CMD_START_EMULATOR 0x03
#define TPMS_CMD_STOP_EMULATOR  0x04
#define TPMS_CMD_SET_CONFIG     0x05
#define TPMS_CMD_GET_CONFIG     0x06
#define TPMS_CMD_GET_STATUS     0x07
#define TPMS_CMD_RESET          0x08

#define TPMS_STATUS_IDLE        0x00
#define TPMS_STATUS_SNIFFING    0x01
#define TPMS_STATUS_EMULATING   0x02

typedef struct {
    uint32_t sensor_id;
    uint8_t  pressure_kpa;
    int8_t   temperature_c;
    uint8_t  battery_ok;
    uint8_t  count;
    int8_t   rssi;
    uint32_t last_seen_ms;
    bool     valid;
} ble_tpms_sensor_t;

typedef struct {
    float    frequency;
    uint8_t  mode;
    uint16_t tx_interval;
    uint8_t  num_sensors;
    uint8_t  reserved[3];
} ble_tpms_config_t;

typedef struct ble_tpms_s ble_tpms_t;
typedef void (*ble_tpms_write_handler_t)(ble_tpms_t *p_tpms, uint8_t cmd, const uint8_t *data, uint16_t len);

struct ble_tpms_s {
    uint16_t                    service_handle;
    ble_gatts_char_handles_t    sensor_char_handles;
    ble_gatts_char_handles_t    config_char_handles;
    ble_gatts_char_handles_t    command_char_handles;
    ble_gatts_char_handles_t    status_char_handles;
    uint8_t                     uuid_type;
    uint16_t                    conn_handle;
    ble_tpms_write_handler_t    write_handler;
};

uint32_t ble_tpms_init(ble_tpms_t *p_tpms, ble_tpms_write_handler_t write_handler);
void     ble_tpms_on_ble_evt(ble_tpms_t *p_tpms, ble_evt_t const *p_ble_evt);
uint32_t ble_tpms_sensor_notify(ble_tpms_t *p_tpms, const ble_tpms_sensor_t *p_sensor);
uint32_t ble_tpms_status_update(ble_tpms_t *p_tpms, uint8_t status);
uint32_t ble_tpms_config_update(ble_tpms_t *p_tpms, const ble_tpms_config_t *p_config);

#endif
