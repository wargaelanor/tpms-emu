// TPMS NRF52840 — main.c (nRF5 SDK, no Arduino)

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "nrf.h"
#include "nrf_gpio.h"
#include "nrf_delay.h"
#include "nrf_drv_saadc.h"
#include "app_error.h"
#include "app_timer.h"
#include "app_scheduler.h"
#include "nrf_sdh.h"
#include "nrf_sdh_ble.h"
#include "nrf_sdh_soc.h"
#include "nrf_ble_gatt.h"
#include "nrf_ble_qwr.h"
#include "nrf_ble_advertising.h"
#include "nrf_log.h"
#include "nrf_log_ctrl.h"
#include "nrf_log_default_backends.h"
#include "custom_board.h"
#include "cc1101.h"
#include "ble_tpms.h"

#define APP_BLE_CONN_CFG_TAG 1
#define APP_BLE_OBS_PRIO     3
#define ADV_INTERVAL         MSEC_TO_UNITS(100, UNIT_0_625_MS)
#define SCHED_QUEUE_SIZE     32
#define SCHED_MAX_EVENT_DATA_SIZE NRF_SDH_BLE_GATT_MAX_MTU_SIZE
#define SENSOR_COUNT         4
#define SNIFFER_MAX_SENSORS  8

static ble_tpms_t m_tpms;
static nrf_ble_gatt_t m_gatt;
static nrf_ble_qwr_t m_qwr;
static uint16_t m_conn_handle = BLE_CONN_HANDLE_INVALID;
static bool m_sniffer_active = false;
static bool m_emulator_active = false;

static ble_tpms_config_t m_config = {
    .frequency = 315.0f, .mode = 0, .tx_interval = 300, .num_sensors = 4
};

static ble_tpms_sensor_t m_sniffer_sensors[SNIFFER_MAX_SENSORS];
static uint8_t m_sniffer_count = 0;

static ble_tpms_sensor_t m_emulator_sensors[SENSOR_COUNT] = {
    {.sensor_id = 0x00001111, .pressure_kpa = 200, .temperature_c = 25, .battery_ok = 1, .valid = true},
    {.sensor_id = 0x00002222, .pressure_kpa = 200, .temperature_c = 25, .battery_ok = 1, .valid = true},
    {.sensor_id = 0x00003333, .pressure_kpa = 200, .temperature_c = 25, .battery_ok = 1, .valid = true},
    {.sensor_id = 0x00004444, .pressure_kpa = 200, .temperature_c = 25, .battery_ok = 1, .valid = true},
};

APP_TIMER_DEF(m_tpms_timer_id);
static nrf_saadc_value_t m_adc_buf[1];
static volatile bool m_adc_done = false;

static void ble_evt_handler(ble_evt_t const *p_ble_evt, void *p_context);
static void tpms_timer_handler(void *p_context);
static void ble_tpms_write_handler(ble_tpms_t *p_tpms, uint8_t cmd, const uint8_t *data, uint16_t len);
static void sniffer_process(void);
static void emulator_send_burst(void);
static void pmv107j_encode(uint32_t id, uint8_t p, int8_t t, uint8_t cnt, uint8_t *buf, uint8_t *len);

static void softdevice_init(void)
{
    uint32_t err = nrf_sdh_enable_request(); APP_ERROR_CHECK(err);
    uint32_t ram = 0;
    err = nrf_sdh_ble_default_cfg_set(APP_BLE_CONN_CFG_TAG, &ram); APP_ERROR_CHECK(err);
    err = nrf_sdh_ble_enable(&ram); APP_ERROR_CHECK(err);
    NRF_SDH_BLE_OBSERVER(m_obs, APP_BLE_OBS_PRIO, ble_evt_handler, NULL);
}

static void gap_params_init(void)
{
    ble_gap_conn_params_t cp = {0};
    ble_gap_conn_sec_mode_t sm;
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&sm);
    APP_ERROR_CHECK(sd_ble_gap_device_name_set(&sm, (const uint8_t*)DEVICE_NAME, strlen(DEVICE_NAME)));
    cp.min_conn_interval = MSEC_TO_UNITS(100, UNIT_1_25_MS);
    cp.max_conn_interval = MSEC_TO_UNITS(200, UNIT_1_25_MS);
    cp.conn_sup_timeout = MSEC_TO_UNITS(4000, UNIT_10_MS);
    APP_ERROR_CHECK(sd_ble_gap_ppcp_set(&cp));
}

static void gatt_init(void) { APP_ERROR_CHECK(nrf_ble_gatt_init(&m_gatt, NULL)); }

static void qwr_init(void)
{
    nrf_ble_qwr_init_t q = {0};
    APP_ERROR_CHECK(nrf_ble_qwr_init(&m_qwr, &q));
}

static void saadc_callback(nrf_drv_saadc_evt_t const *p) { if (p->type == NRF_DRV_SAADC_EVT_DONE) m_adc_done = true; }

static void saadc_init(void)
{
    nrf_saadc_channel_config_t cc = NRF_DRV_SAADC_DEFAULT_CHANNEL_CONFIG_SE(NRF_SAADC_INPUT_AIN2);
    APP_ERROR_CHECK(nrf_drv_saadc_init(NULL, saadc_callback));
    APP_ERROR_CHECK(nrf_drv_saadc_channel_init(BATTERY_ADC_CHANNEL, &cc));
}

static uint16_t read_battery_mv(void)
{
    nrf_drv_saadc_buffer_convert(m_adc_buf, 1);
    m_adc_done = false;
    nrf_drv_saadc_sample();
    uint32_t t = 1000;
    while (!m_adc_done && t-- > 0) nrf_delay_us(100);
    return (uint16_t)((m_adc_buf[0] * 3600UL * BATTERY_VOLTAGE_DIV) / 1024);
}

static void on_adv_evt(ble_adv_evt_t adv_evt) { (void)adv_evt; }

static void advertising_init(void)
{
    ble_advertising_init_t init = {0};
    init.advdata.name_type = BLE_ADVDATA_FULL_NAME;
    init.advdata.flags = BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE;
    init.config.ble_adv_fast_enabled = true;
    init.config.ble_adv_fast_interval = ADV_INTERVAL;
    init.config.ble_adv_fast_timeout = 0;
    init.evt_handler = on_adv_evt;
    APP_ERROR_CHECK(ble_advertising_init(&m_advertising, &init));
    ble_advertising_conn_cfg_tag_set(&m_advertising, APP_BLE_CONN_CFG_TAG);
}

static void ble_tpms_write_handler(ble_tpms_t *p_tpms, uint8_t cmd, const uint8_t *data, uint16_t len)
{
    (void)data; (void)len;
    switch (cmd) {
        case TPMS_CMD_START_SNIFFER:
            m_sniffer_active = true; m_sniffer_count = 0;
            memset(m_sniffer_sensors, 0, sizeof(m_sniffer_sensors));
            cc1101_power_on(); cc1101_set_frequency(m_config.frequency);
            cc1101_set_modulation(0); cc1101_set_data_rate(0x59);
            cc1101_set_deviation(0x47); cc1101_set_sync_mode(0);
            cc1101_set_rx_config(); cc1101_flush_rx_fifo(); cc1101_rx();
            ble_tpms_status_update(p_tpms, TPMS_STATUS_SNIFFING);
            break;
        case TPMS_CMD_STOP_SNIFFER:
            m_sniffer_active = false; cc1101_idle(); cc1101_power_off();
            ble_tpms_status_update(p_tpms, TPMS_STATUS_IDLE);
            break;
        case TPMS_CMD_START_EMULATOR:
            m_emulator_active = true;
            cc1101_power_on(); cc1101_set_frequency(m_config.frequency);
            cc1101_set_modulation(0); cc1101_set_data_rate(0x59);
            cc1101_set_deviation(0x47); cc1101_set_sync_mode(0); cc1101_set_tx_config();
            ble_tpms_status_update(p_tpms, TPMS_STATUS_EMULATING);
            break;
        case TPMS_CMD_STOP_EMULATOR:
            m_emulator_active = false; cc1101_idle(); cc1101_power_off();
            ble_tpms_status_update(p_tpms, TPMS_STATUS_IDLE);
            break;
        case TPMS_CMD_SET_CONFIG:
            if (len >= sizeof(ble_tpms_config_t)) memcpy(&m_config, data, sizeof(ble_tpms_config_t));
            break;
        case TPMS_CMD_GET_CONFIG:
            ble_tpms_config_update(p_tpms, &m_config); break;
        case TPMS_CMD_GET_STATUS:
            ble_tpms_status_update(p_tpms, m_sniffer_active ? TPMS_STATUS_SNIFFING : m_emulator_active ? TPMS_STATUS_EMULATING : TPMS_STATUS_IDLE);
            break;
        case TPMS_CMD_RESET:
            NVIC_SystemReset(); break;
    }
}

static void ble_evt_handler(ble_evt_t const *p, void *ctx)
{
    (void)ctx;
    switch (p->header.evt_id) {
        case BLE_GAP_EVT_CONNECTED:
            m_conn_handle = p->evt.gap_evt.conn_handle;
            nrf_ble_qwr_conn_handle_assign(&m_qwr, m_conn_handle);
            break;
        case BLE_GAP_EVT_DISCONNECTED:
            m_conn_handle = BLE_CONN_HANDLE_INVALID;
            ble_advertising_start(&m_advertising, BLE_ADV_MODE_FAST);
            break;
    }
    ble_tpms_on_ble_evt(&m_tpms, p);
}

static void sniffer_process(void)
{
    if (cc1101_get_marc_state() != 0x0D) {
        cc1101_idle(); cc1101_flush_rx_fifo(); cc1101_calibrate(); cc1101_rx();
        return;
    }
    uint8_t rx = cc1101_get_rx_bytes();
    if (rx < 10) return;
    uint8_t buf[64]; uint8_t len = rx > 64 ? 64 : rx;
    int8_t rssi = cc1101_get_rssi();
    cc1101_read_burst(CC1101_RX_FIFO, buf, len);
    cc1101_flush_rx_fifo();
    if (rssi > -95 && len >= 4) {
        uint32_t id = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) | ((uint32_t)buf[2] << 8) | buf[3];
        bool found = false;
        for (uint8_t i = 0; i < m_sniffer_count; i++) {
            if (m_sniffer_sensors[i].sensor_id == id) {
                m_sniffer_sensors[i].rssi = rssi; found = true; break;
            }
        }
        if (!found && m_sniffer_count < SNIFFER_MAX_SENSORS) {
            m_sniffer_sensors[m_sniffer_count].sensor_id = id;
            m_sniffer_sensors[m_sniffer_count].rssi = rssi;
            m_sniffer_sensors[m_sniffer_count].valid = true;
            ble_tpms_sensor_notify(&m_tpms, &m_sniffer_sensors[m_sniffer_count]);
            m_sniffer_count++;
        }
    }
    cc1101_rx();
}

static void emulator_send_burst(void)
{
    for (uint8_t i = 0; i < m_config.num_sensors && i < SENSOR_COUNT; i++) {
        uint8_t pkt[16], pkt_len;
        m_emulator_sensors[i].count++;
        pmv107j_encode(m_emulator_sensors[i].sensor_id, m_emulator_sensors[i].pressure_kpa,
                       m_emulator_sensors[i].temperature_c, m_emulator_sensors[i].count, pkt, &pkt_len);
        cc1101_send_packet(pkt, pkt_len);
        nrf_delay_ms(5);
    }
}

static void pmv107j_encode(uint32_t id, uint8_t p, int8_t t, uint8_t cnt, uint8_t *buf, uint8_t *len)
{
    uint8_t i = 0;
    buf[i++] = 0xFC;
    buf[i++] = (id >> 24) & 0xFF; buf[i++] = (id >> 16) & 0xFF;
    buf[i++] = (id >> 8) & 0xFF;  buf[i++] = id & 0xFF;
    buf[i++] = 1 | ((cnt & 3) << 1);
    buf[i++] = p + 70;
    buf[i++] = (uint8_t)(t + 40);
    buf[i++] = 0x00; // CRC placeholder
    *len = i;
}

static void tpms_timer_handler(void *p_context)
{
    (void)p_context;
    if (m_emulator_active) emulator_send_burst();
    if (m_sniffer_active) sniffer_process();
}

int main(void)
{
    APP_ERROR_CHECK(NRF_LOG_INIT(NULL));
    NRF_LOG_DEFAULT_BACKENDS_INIT();
    NRF_LOG_INFO("TPMS NRF52840 Starting");

    APP_ERROR_CHECK(app_timer_init());
    APP_SCHED_INIT(SCHED_MAX_EVENT_DATA_SIZE, SCHED_QUEUE_SIZE);
    softdevice_init();
    gap_params_init();
    gatt_init();
    qwr_init();
    APP_ERROR_CHECK(ble_tpms_init(&m_tpms, ble_tpms_write_handler));
    advertising_init();
    saadc_init();
    cc1101_init();
    cc1101_power_on();
    NRF_LOG_INFO("CC1101 MARCSTATE=0x%02x", cc1101_get_marc_state());

    ble_advertising_start(&m_advertising, BLE_ADV_MODE_FAST);
    APP_ERROR_CHECK(app_timer_create(&m_tpms_timer_id, APP_TIMER_MODE_REPEATED, tpms_timer_handler));
    APP_ERROR_CHECK(app_timer_start(m_tpms_timer_id, APP_TIMER_TICKS(1000)));

    NRF_LOG_INFO("TPMS NRF52840 Ready");

    for (;;) {
        app_sched_execute();
        if (NRF_LOG_PROCESS() == false) { /* idle */ }
    }
}
