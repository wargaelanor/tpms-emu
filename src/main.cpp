/**
 * TPMS Emulator / Sniffer for ProMicro nRF52840 V1940 (Nice!Nano clone)
 *
 * Based on TPMS-Emulator v7.3 (ESP32-C3 / ESP8266).
 * Replaces WiFi/Web UI with BLE UART + Android app.
 *
 * Protocol: PMV-107J (Pacific Industrial) on 315 MHz / 433 MHz
 *
 * Pinout (ProMicro nRF52840 V1940 / Nice!Nano clone, Feather variant):
 *   Custom SPI MISO -> D29 (P0.17, labeled "017" on board)
 *   Custom SPI MOSI -> D20 (P0.29, labeled "029" on board)
 *   Custom SPI SCK  -> D21 (P0.31, labeled "031" on board)
 *   CC1101 CS       -> D2  (P0.10, labeled "010" on board)
 *   CC1101 GDO0     -> D11 (P0.06, labeled "006" on board)
 *   CC1101 GDO2     -> D12 (P0.08, labeled "008" on board)
 *   CC1101 POWER    -> D28 (P0.20, labeled "020" on board)
 *   Status LED      -> D24 (P0.15, onboard LED)
 *   Battery ADC     -> A4  (P0.02, labeled "002" on board, AIN4)
 *
 * NOTE: ProMicro nRF52840 V1940 may have different physical pin labels.
 * Adjust macros below to match your wiring.
 */

#include <Arduino.h>
#include <SPI.h>
#include <string.h>
#include <bluefruit.h>
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
#include "CC1101.h"

using namespace Adafruit_LittleFS_Namespace;

// ============================================================================
// Pin Definitions (ProMicro nRF52840 V1940 / Nice!Nano clone)
// ============================================================================
#define PIN_CC1101_CS      2    // D2  = P0.10 (labeled "010" on board)
#define PIN_CC1101_GDO0    11   // D11 = P0.06 (labeled "006" on board)
#define PIN_CC1101_GDO2    12   // D12 = P0.08 (labeled "008" on board)
#define PIN_CC1101_POWER   28   // D28 = P0.20 (labeled "020" on board)
#define PIN_LED_STATUS     24   // D24 = P0.15 (onboard LED)
#define PIN_BATTERY_ADC    A4   // D18 = P0.02 (labeled "002" on board, AIN4)

// ============================================================================
// Configuration Constants
// ============================================================================
#define TPMS_FREQ_315            315.0f
#define TPMS_FREQ_433            433.92f
#define TPMS_FREQ_DEFAULT        TPMS_FREQ_315
#define TPMS_DEFAULT_DATARATE    10000
#define TPMS_DEFAULT_DEVIATION   38.0f
#define TPMS_DEFAULT_POWER       5
#define TPMS_TX_INTERVAL_MS      200
#define TPMS_POWER_MIN           1
#define TPMS_POWER_MAX           7
#define TPMS_BURST_COUNT         5
#define TPMS_MAX_TX_INTERVAL     900
#define TPMS_NUM_SENSORS         4
#define EEPROM_MAGIC             0xB0

#define BATT_FULL_MV             4200
#define BATT_LOW_MV              3300
#define BATT_CRITICAL_MV         3000
#define BATT_DEFAULT_MAH         3000

// ============================================================================
// EEPROM Layout
// ============================================================================
#define EE_MAGIC       0
#define EE_DATARATE    1
#define EE_DEVIATION   5
#define EE_POWER       7
#define EE_MANCH_EN    8
#define EE_TX_ENABLED  9
#define EE_TX_INTERVAL 10
#define EE_TX_PACKETS  12
#define EE_FREQ        13
#define EE_SENSORS     14
#define EE_LICENSE     42
#define EE_TRIAL_SEC   43
#define EE_BATT_MAH    47
#define EE_TOTAL       49

static uint8_t ee_buf[EE_TOTAL];

static uint32_t ee_read32(int off) {
    return ((uint32_t)ee_buf[off] | ((uint32_t)ee_buf[off+1]<<8) |
            ((uint32_t)ee_buf[off+2]<<16) | ((uint32_t)ee_buf[off+3]<<24));
}
static void ee_write32(int off, uint32_t v) {
    ee_buf[off]=v; ee_buf[off+1]=v>>8; ee_buf[off+2]=v>>16; ee_buf[off+3]=v>>24;
}
static uint16_t ee_read16(int off) {
    return ((uint16_t)ee_buf[off] | ((uint16_t)ee_buf[off+1]<<8));
}
static void ee_write16(int off, uint16_t v) {
    ee_buf[off]=v; ee_buf[off+1]=v>>8;
}

static void eeprom_load() {
    memset(ee_buf, 0xFF, EE_TOTAL);
    File f = InternalFS.open("tpms_cfg", FILE_O_READ);
    if (f) {
        f.read(ee_buf, EE_TOTAL);
        f.close();
    }
}

static void eeprom_commit() {
    InternalFS.remove("tpms_cfg");
    File f = InternalFS.open("tpms_cfg", FILE_O_WRITE);
    if (f) {
        f.seek(0);
        f.write(ee_buf, EE_TOTAL);
        f.truncate();
        f.close();
    }
}

// ============================================================================
// BLE Objects
// ============================================================================
BLEUart bleUart;

// ============================================================================
// License System (simplified — accept any 8-char HEX key)
// ============================================================================
static bool is_licensed() {
    return ee_buf[EE_LICENSE] == 0xFF;
}

static uint32_t trial_unwritten = 0;

static uint32_t get_trial_seconds() {
    uint32_t v = ee_read32(EE_TRIAL_SEC);
    uint32_t base = (v == 0xFFFFFFFF) ? 0 : v;
    uint32_t total = base + trial_unwritten;
    return total > 86400 ? 86400 : total;
}

static void flush_trial() {
    if (trial_unwritten == 0) return;
    uint32_t base = ee_read32(EE_TRIAL_SEC);
    base = (base == 0xFFFFFFFF) ? 0 : base;
    uint32_t total = base + trial_unwritten;
    if (total > 86400) total = 86400;
    ee_write32(EE_TRIAL_SEC, total);
    eeprom_commit();
    trial_unwritten = 0;
}

static void add_trial_seconds(uint32_t s) {
    trial_unwritten += s;
    uint32_t total = get_trial_seconds();
    if (trial_unwritten >= 60 || total >= 86400) {
        flush_trial();
    }
}

static bool is_trial_expired() {
    return get_trial_seconds() >= 86400;
}

static bool can_sniff() {
    return is_licensed() || !is_trial_expired();
}

// ============================================================================
// PMV-107J Protocol Defines
// ============================================================================
#define PMV107J_PAYLOAD_BITS     58
#define PMV107J_TOTAL_BITS       66
#define PMV107J_PRESSURE_OFFSET  40
#define PMV107J_PRESSURE_KPA_SCALE 2.48f
#define PMV107J_TEMP_OFFSET      40
#define PMV107J_SETTLE_BITS      16
#define PMV107J_PREAMBLE_BITS    6
#define PMV107J_TRAILER_BITS     6

// ============================================================================
// Data Structures
// ============================================================================
struct SensorConfig {
    uint32_t sensor_id;
    uint8_t  pressure_kpa;
    int8_t   temperature_c;
    uint8_t  battery_ok;
    uint8_t  flags;
} __attribute__((packed));

struct SystemConfig {
    uint8_t  magic;
    uint32_t datarate;
    uint16_t deviation;
    uint8_t  power;
    uint8_t  manchester_en;
    uint8_t  tx_enabled;
    uint8_t  freq;
    uint8_t  reserved[3];
    uint16_t autoTxInterval;
    uint8_t  autoTxPackets;
    uint8_t  reserved2;
    SensorConfig sensors[TPMS_NUM_SENSORS];
    uint16_t battMah;
} __attribute__((packed));

// ============================================================================
// Globals
// ============================================================================
CC1101 cc1101(PIN_CC1101_CS, PIN_CC1101_GDO0, PIN_CC1101_GDO2);
SystemConfig config;

bool     autoTxEnabled = true;
uint16_t autoTxInterval = 60;
uint8_t  autoTxPackets = 2;
uint32_t autoTxLastMs = 0;
bool     autoTxRunning = false;

bool     snifferActive = false;
uint32_t snifferStartMs = 0;
uint32_t sniffer_discovery_ms = 0;

#define  SNIFFER_TIMEOUT_MS 3600000
#define  SNIFFER_MAX_SENSORS 4

struct SnifferResult {
    uint32_t sensor_id;
    uint8_t  pressure_kpa;
    int8_t   temperature_c;
    uint8_t  battery_ok;
    uint32_t last_seen_ms;
    bool     valid;
} sniffer_sensors[SNIFFER_MAX_SENSORS];
uint8_t sniffer_count = 0;

uint32_t last_tx_time = 0;
uint8_t  tx_counter = 0;
uint8_t  burst_count = 0;

const uint32_t DEFAULT_SENSOR_IDS[TPMS_NUM_SENSORS] = {
    0x00298088, 0x0466E088, 0x0D784088, 0x0C765088
};

const char* SENSOR_LABELS[TPMS_NUM_SENSORS] = { "PL", "PP", "ZL", "ZP" };

static char dbg_last_id[96] = "";

// Function prototypes
void init_config();
void load_config();
void save_config();
void init_cc1101();
void cc1101_send_raw(const uint8_t *data, uint8_t len);
void send_pmv107j_sensor(uint8_t sensor_idx);
bool set_cc1101_data_rate(uint32_t baud);
bool set_cc1101_deviation(float khz);
void set_cc1101_power(int8_t pa_index);
void set_cc1101_manchester(bool enable);
void sniffer_start();
void sniffer_stop();
void sniffer_loop();
bool sniffer_decode_packet(const uint8_t *raw, uint8_t len, uint32_t *id, uint8_t *pressure, int8_t *temp);
uint16_t dm_decode(const uint8_t *dm_bits, uint16_t dm_len, uint8_t *out_bits);
uint8_t calculate_crc8_pmv(const uint8_t *data, uint8_t len);
void cc1101_power_on();
void cc1101_power_off();
void send_burst_all();
void runAutoTx();
static String buildStatusJson();
void bleuart_rx_callback(uint16_t conn_handle);
void processCommand(const String &msg);
static String jsonGetString(const String &json, const char *key);
static long jsonGetLong(const String &json, const char *key);
void uartSendLog(const String &msg);
void uartBroadcastStatus();

// Deferred command processing from BLE callback to main loop
static String bleCommandBuffer;
static volatile bool bleCommandReady = false;



// Bit helpers
static inline void set_bit(uint8_t *buf, uint16_t pos, bool val) {
    uint16_t byte_idx = pos / 8;
    uint8_t bit_idx = 7 - (pos % 8);
    if (val) buf[byte_idx] |= (1 << bit_idx);
}
static inline bool get_bit(const uint8_t *buf, uint16_t pos) {
    uint16_t byte_idx = pos / 8;
    uint8_t bit_idx = 7 - (pos % 8);
    return (buf[byte_idx] >> bit_idx) & 1;
}

// CRC-8
uint8_t calculate_crc8_pmv(const uint8_t *data, uint8_t len) {
    uint8_t crc = 0x00;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x80) crc = (crc << 1) ^ 0x13;
            else crc = crc << 1;
        }
    }
    return crc;
}

// ============================================================================
// Battery
// ============================================================================
uint16_t readBatteryMv() {
    int raw = analogRead(PIN_BATTERY_ADC);
    // nRF52840 has 12-bit ADC (0-4095), default 3.3V reference
    // With 1:2 divider: Vbat = raw * 3.3V / 4095 * 2 * 1000
    return (uint16_t)((uint32_t)raw * 3300UL * 2 / 4095);
}

uint8_t getBatteryPercent() {
    uint16_t mv = readBatteryMv();
    if (mv >= BATT_FULL_MV) return 100;
    if (mv <= BATT_CRITICAL_MV) return 0;
    return (uint8_t)((mv - BATT_CRITICAL_MV) * 100 / (BATT_FULL_MV - BATT_CRITICAL_MV));
}

bool isBatteryLow() { return readBatteryMv() < BATT_LOW_MV; }
bool isBatteryCritical() { return readBatteryMv() < BATT_CRITICAL_MV; }

// ============================================================================
// CC1101 Power Control
// ============================================================================
void cc1101_power_on() {
    digitalWrite(PIN_CC1101_POWER, HIGH);
    delay(2);
}

void cc1101_power_off() {
    cc1101.setIdleState();
    delay(1);
    digitalWrite(PIN_CC1101_CS, HIGH);
    SPI.end();
    pinMode(PIN_CC1101_CS, OUTPUT);
    digitalWrite(PIN_CC1101_CS, HIGH);
    digitalWrite(PIN_CC1101_POWER, LOW);
}

// ============================================================================
// CC1101 Init
// ============================================================================
void init_cc1101() {
    cc1101.init();
    cc1101.setFreq((config.freq == 1) ? TPMS_FREQ_433 : TPMS_FREQ_315);
    cc1101.setFreqConfig((config.freq == 1) ? TPMS_FREQ_433 : TPMS_FREQ_315);
    cc1101.setModulation(0);
    set_cc1101_data_rate(config.datarate);
    set_cc1101_deviation(config.deviation / 10.0f);
    set_cc1101_manchester(0);
    config.manchester_en = 0;
    set_cc1101_power(config.power);
    cc1101.setSyncMode(0);
    cc1101.writeReg(CC1101_PKTCTRL0, 0x00);
    cc1101.writeReg(CC1101_PKTCTRL1, 0x00);
}

bool set_cc1101_data_rate(uint32_t baud) {
    if (baud < 600 || baud > 500000) return false;
    cc1101.setMHZOsc(26.0f);
    bool result = cc1101.setDRate(baud);
    if (result) config.datarate = baud;
    return result;
}

bool set_cc1101_deviation(float khz) {
    if (khz < 1.58f || khz > 380.0f) return false;
    bool result = cc1101.setDeviation(khz);
    if (result) config.deviation = (uint16_t)(khz * 10);
    return result;
}

void set_cc1101_power(int8_t pa_index) {
    pa_index = constrain(pa_index, TPMS_POWER_MIN, TPMS_POWER_MAX);
    cc1101.setPA(pa_index);
    config.power = pa_index;
}

void set_cc1101_manchester(bool enable) {
    cc1101.setManc(enable ? 1 : 0);
    config.manchester_en = enable ? 1 : 0;
}

// ============================================================================
// PMV-107J Encoder
// ============================================================================
uint16_t build_pmv107j_payload(uint8_t sensor_idx, uint8_t *bit_buf) {
    if (sensor_idx >= TPMS_NUM_SENSORS) return 0;
    SensorConfig *s = &config.sensors[sensor_idx];
    memset(bit_buf, 0, 16);
    uint16_t pos = 0;

    uint32_t id = s->sensor_id;
    for (int8_t i = 27; i >= 0; i--) set_bit(bit_buf, pos++, (id >> i) & 1);
    set_bit(bit_buf, pos++, 0);

    uint8_t cnt = (tx_counter % 3) + 1;
    set_bit(bit_buf, pos++, (cnt >> 1) & 1);
    set_bit(bit_buf, pos++, cnt & 1);

    set_bit(bit_buf, pos++, 0);
    set_bit(bit_buf, pos++, 0);
    set_bit(bit_buf, pos++, 0);

    uint8_t pressure_byte = (uint8_t)((float)s->pressure_kpa / PMV107J_PRESSURE_KPA_SCALE + PMV107J_PRESSURE_OFFSET);
    for (int8_t i = 7; i >= 0; i--) set_bit(bit_buf, pos++, (pressure_byte >> i) & 1);

    uint8_t inv_pressure = pressure_byte ^ 0xFF;
    for (int8_t i = 7; i >= 0; i--) set_bit(bit_buf, pos++, (inv_pressure >> i) & 1);

    uint8_t temp_byte = (uint8_t)(s->temperature_c + PMV107J_TEMP_OFFSET);
    for (int8_t i = 7; i >= 0; i--) set_bit(bit_buf, pos++, (temp_byte >> i) & 1);

    uint8_t crc_buf[8];
    memset(crc_buf, 0, 8);
    for (uint16_t i = 0; i < 58; i++) set_bit(crc_buf, 6 + i, get_bit(bit_buf, i));
    uint8_t crc = calculate_crc8_pmv(crc_buf, 8);

    for (int8_t i = 7; i >= 0; i--) set_bit(bit_buf, pos++, (crc >> i) & 1);

    tx_counter = (tx_counter + 1) & 0xFF;
    return pos;
}

uint16_t differential_manchester_encode(const uint8_t *data_bits, uint16_t num_bits, uint8_t *out_buf) {
    memset(out_buf, 0, 40);
    uint16_t out_pos = 0;
    uint8_t state = 0;
    for (uint16_t i = 0; i < num_bits; i++) {
        bool bit = get_bit(data_bits, i);
        bool same = (bit == state);
        if (same) {
            set_bit(out_buf, out_pos++, 1);
            set_bit(out_buf, out_pos++, 0);
            state = 0;
        } else {
            set_bit(out_buf, out_pos++, 0);
            set_bit(out_buf, out_pos++, 1);
            state = 1;
        }
    }
    return out_pos;
}

void cc1101_send_raw(const uint8_t *data, uint8_t len) {
    cc1101.setIdleState();
    delay(1);
    cc1101.sendCommand(CC1101_SCAL);
    delay(3);
    cc1101.writeReg(CC1101_PKTCTRL0, 0x00);
    cc1101.writeReg(CC1101_PKTLEN, len);
    cc1101.writeReg(CC1101_PKTCTRL1, 0x00);
    cc1101.setManc(0);
    cc1101.flushTxFifo();
    delay(1);
    cc1101.writeBurstReg(CC1101_TX_FIFO, data, len);
    cc1101.setTxState();
    delay(3);
    uint8_t state = cc1101.getChipState();
    if (state == 0x13) {
        unsigned int packet_time_ms = (unsigned int)(len * 10 / 8) + 10;
        delay(packet_time_ms);
    }
    cc1101.setIdleState();
    delay(1);
    cc1101.flushTxFifo();
}

void send_pmv107j_sensor(uint8_t sensor_idx) {
    if (sensor_idx >= TPMS_NUM_SENSORS) return;
    uint8_t payload_bits[16];
    uint16_t payload_len = build_pmv107j_payload(sensor_idx, payload_bits);
    if (payload_len != PMV107J_TOTAL_BITS) return;

    uint8_t dm_input[16];
    memset(dm_input, 0, 16);
    uint16_t dm_in_pos = 0;
    set_bit(dm_input, dm_in_pos++, 1);
    for (uint16_t i = 0; i < payload_len; i++) set_bit(dm_input, dm_in_pos++, get_bit(payload_bits, i));
    set_bit(dm_input, dm_in_pos++, 1);

    uint8_t dm_output[40];
    uint16_t dm_out_len = differential_manchester_encode(dm_input, dm_in_pos, dm_output);

    uint8_t tx_buf[30];
    memset(tx_buf, 0, 30);
    uint16_t tx_pos = 0;
    tx_pos = PMV107J_SETTLE_BITS;
    for (uint8_t i = 0; i < PMV107J_PREAMBLE_BITS; i++) {
        if (i < 5) set_bit(tx_buf, tx_pos + i, 1);
    }
    tx_pos += PMV107J_PREAMBLE_BITS;
    for (uint16_t i = 0; i < dm_out_len; i++) set_bit(tx_buf, tx_pos + i, get_bit(dm_output, i));
    tx_pos += dm_out_len;
    tx_pos += PMV107J_TRAILER_BITS;

    uint8_t tx_bytes = (tx_pos + 7) / 8;
    cc1101_send_raw(tx_buf, tx_bytes);
}

void send_burst_all() {
    for (uint8_t s = 0; s < TPMS_NUM_SENSORS; s++) {
        if (!(config.sensors[s].flags & 0x01)) continue;
        for (uint8_t i = 0; i < autoTxPackets; i++) {
            send_pmv107j_sensor(s);
            if (i < autoTxPackets - 1) delay(TPMS_TX_INTERVAL_MS);
        }
    }
}

// ============================================================================
// Sniffer Decode
// ============================================================================
uint16_t dm_decode(const uint8_t *dm_bits, uint16_t dm_len, uint8_t *out_bits) {
    uint16_t out_pos = 0;
    uint8_t prev_level = 0;
    for (uint16_t i = 0; i < dm_len && out_pos < 128; i += 2) {
        bool b1 = (dm_bits[i / 8] >> (7 - (i % 8))) & 1;
        bool b2 = (dm_bits[(i + 1) / 8] >> (7 - ((i + 1) % 8))) & 1;
        if (b1 == b2) continue;
        if (b1 == prev_level) out_bits[out_pos / 8] |= (1 << (7 - (out_pos % 8)));
        else out_bits[out_pos / 8] &= ~(1 << (7 - (out_pos % 8)));
        out_pos++;
        prev_level = b2;
    }
    return out_pos;
}

bool sniffer_decode_packet(const uint8_t *raw, uint8_t len, uint32_t *id, uint8_t *pressure, int8_t *temp) {
    if (len < 5) return false;
    uint16_t preamble_end = 0;
    bool found = false;
    for (int16_t i = (int16_t)(len * 8) - 7; i >= 0; i--) {
        uint8_t byte_idx = i / 8;
        uint8_t bit_idx = 7 - (i % 8);
        bool b0 = (raw[byte_idx] >> bit_idx) & 1;
        bool b1 = (raw[(i + 1) / 8] >> (7 - ((i + 1) % 8))) & 1;
        bool b2 = (raw[(i + 2) / 8] >> (7 - ((i + 2) % 8))) & 1;
        bool b3 = (raw[(i + 3) / 8] >> (7 - ((i + 3) % 8))) & 1;
        bool b4 = (raw[(i + 4) / 8] >> (7 - ((i + 4) % 8))) & 1;
        bool b5 = (raw[(i + 5) / 8] >> (7 - ((i + 5) % 8))) & 1;
        if (b0 && b1 && b2 && b3 && b4 && !b5) {
            uint16_t bits_after = (len * 8) - (i + 6);
            if (bits_after >= 136) {
                preamble_end = i + 6;
                found = true;
                break;
            }
        }
    }
    if (!found) return false;

    uint8_t dm_data[80];
    memset(dm_data, 0, sizeof(dm_data));
    uint16_t dm_bit_count = 0;
    uint16_t max_bits = (len * 8) - preamble_end;
    if (max_bits > 136) max_bits = 136;
    for (uint16_t i = preamble_end; i < preamble_end + max_bits; i++) {
        uint8_t byte_idx = i / 8;
        uint8_t bit_idx = 7 - (i % 8);
        bool bit = (raw[byte_idx] >> bit_idx) & 1;
        if (bit) dm_data[dm_bit_count / 8] |= (1 << (7 - (dm_bit_count % 8)));
        dm_bit_count++;
    }

    uint8_t decoded[40];
    memset(decoded, 0, sizeof(decoded));
    uint16_t decoded_len = dm_decode(dm_data, dm_bit_count, decoded);
    if (decoded_len < 68) return false;

#define DECODE_OFFSET 1
    *id = 0;
    for (int8_t i = 0; i < 28; i++) {
        uint8_t byte_idx = (DECODE_OFFSET + i) / 8;
        uint8_t bit_idx = 7 - ((DECODE_OFFSET + i) % 8);
        bool bit = (decoded[byte_idx] >> bit_idx) & 1;
        *id = (*id << 1) | bit;
    }

    uint8_t p_byte = 0;
    for (int8_t i = 0; i < 8; i++) {
        uint8_t byte_idx = (DECODE_OFFSET + 34 + i) / 8;
        uint8_t bit_idx = 7 - ((DECODE_OFFSET + 34 + i) % 8);
        bool bit = (decoded[byte_idx] >> bit_idx) & 1;
        p_byte = (p_byte << 1) | bit;
    }
    *pressure = (uint8_t)((float)(p_byte - 40) * 2.48f);

    uint8_t t_byte = 0;
    for (int8_t i = 0; i < 8; i++) {
        uint8_t byte_idx = (DECODE_OFFSET + 50 + i) / 8;
        uint8_t bit_idx = 7 - ((DECODE_OFFSET + 50 + i) % 8);
        bool bit = (decoded[byte_idx] >> bit_idx) & 1;
        t_byte = (t_byte << 1) | bit;
    }
    *temp = (int8_t)(t_byte - 40);

    uint8_t crc_buf[8];
    memset(crc_buf, 0, 8);
    for (uint16_t i = 0; i < 58; i++) {
        uint8_t byte_idx = (DECODE_OFFSET + i) / 8;
        uint8_t bit_idx = 7 - ((DECODE_OFFSET + i) % 8);
        bool bit = (decoded[byte_idx] >> bit_idx) & 1;
        if (bit) crc_buf[(6 + i) / 8] |= (1 << (7 - ((6 + i) % 8)));
    }
    uint8_t crc = calculate_crc8_pmv(crc_buf, 8);

    uint8_t rx_crc = 0;
    for (int8_t i = 0; i < 8; i++) {
        uint8_t byte_idx = (DECODE_OFFSET + 58 + i) / 8;
        uint8_t bit_idx = 7 - ((DECODE_OFFSET + 58 + i) % 8);
        bool bit = (decoded[byte_idx] >> bit_idx) & 1;
        rx_crc = (rx_crc << 1) | bit;
    }
    return crc == rx_crc;
}

// ============================================================================
// Sniffer Mode
// ============================================================================
void sniffer_start() {
    if (!can_sniff()) {
        uartSendLog("[SNIFFER] BLOCKED - trial expired, license required");
        return;
    }
    uartSendLog("[SNIFFER] Starting...");
    snifferActive = true;
    snifferStartMs = millis();
    sniffer_count = 0;
    memset(sniffer_sensors, 0, sizeof(sniffer_sensors));

    float freq = (config.freq == 1) ? TPMS_FREQ_433 : TPMS_FREQ_315;
    cc1101.setIdleState();
    delay(1);
    cc1101.setFreqConfig(freq);
    cc1101.setFreq(freq);
    cc1101.setRxConfig();
    cc1101.setModulation(0);
    cc1101.setSyncMode(0);
    set_cc1101_data_rate(config.datarate);
    set_cc1101_deviation(38.0f);
    set_cc1101_manchester(0);
    cc1101.sendCommand(CC1101_SCAL);
    delay(3);
    cc1101.flushRxFifo();
    cc1101.setRxState();

    char buf[80];
    snprintf(buf, sizeof(buf), "[SNIFFER] Freq=%.0f MHz, DR=%lu, Dev=38kHz", freq, (unsigned long)config.datarate);
    uartSendLog(buf);
    uartBroadcastStatus();
}

void sniffer_stop() {
    if (!snifferActive) return;
    snifferActive = false;
    cc1101.setIdleState();
    delay(1);
    char buf[64];
    snprintf(buf, sizeof(buf), "[SNIFFER] Stopped - %d sensors found", sniffer_count);
    uartSendLog(buf);
    if (sniffer_count > 0) {
        snifferStartMs = 0xFFFFFFFF;
        uartSendLog("[SNIFFER] Sensors found. Use 'sniff_apply' to save.");
    } else {
        snifferStartMs = millis() + 10000;
    }
    uartBroadcastStatus();
}

void sniffer_loop() {
    if (!snifferActive) return;
    if (millis() - snifferStartMs > SNIFFER_TIMEOUT_MS) {
        uartSendLog("[SNIFFER] Timeout - stopping");
        sniffer_stop();
        return;
    }

    static uint8_t acc_buf[256];
    static uint16_t acc_len = 0;
    static bool rxRunning = false;

    if (!rxRunning) {
        cc1101.setIdleState();
        delay(1);
        cc1101.flushRxFifo();
        cc1101.sendCommand(CC1101_SCAL);
        delay(3);
        cc1101.setRxState();
        rxRunning = true;
        acc_len = 0;
    }

    uint8_t marc = cc1101.getMarcState();
    if (marc != 0x0D) {
        cc1101.setIdleState();
        delay(1);
        cc1101.flushRxFifo();
        cc1101.sendCommand(CC1101_SCAL);
        delay(3);
        cc1101.setRxState();
        acc_len = 0;
    }

    uint8_t rx_bytes = cc1101.getRxBytes() & 0x7F;
    if (rx_bytes > 0) {
        if (rx_bytes > 64) rx_bytes = 64;
        if (acc_len + rx_bytes <= sizeof(acc_buf)) {
            cc1101.readBurstReg(CC1101_RX_FIFO, acc_buf + acc_len, rx_bytes);
            acc_len += rx_bytes;
        }
    }

    if (acc_len >= 10) {
        int8_t rssi = cc1101.getRssi();
        if (rssi > -95) {
            uint32_t id = 0;
            uint8_t pressure = 0;
            int8_t temp = 0;
            if (sniffer_decode_packet(acc_buf, acc_len, &id, &pressure, &temp)) {
                char buf[128];
                snprintf(buf, sizeof(buf), "*** DECODE OK: ID=%08lx P=%d T=%d RSSI=%d",
                         (unsigned long)id, pressure, temp, rssi);
                uartSendLog(buf);

                bool found = false;
                for (uint8_t i = 0; i < sniffer_count; i++) {
                    if (sniffer_sensors[i].sensor_id == id) {
                        sniffer_sensors[i].last_seen_ms = millis();
                        found = true;
                        break;
                    }
                }
                if (!found && sniffer_count < SNIFFER_MAX_SENSORS) {
                    sniffer_sensors[sniffer_count].sensor_id = id;
                    sniffer_sensors[sniffer_count].pressure_kpa = pressure;
                    sniffer_sensors[sniffer_count].temperature_c = temp;
                    sniffer_sensors[sniffer_count].battery_ok = 1;
                    sniffer_sensors[sniffer_count].last_seen_ms = millis();
                    sniffer_sensors[sniffer_count].valid = true;
                    sniffer_count++;
                    sniffer_discovery_ms = millis();
                    snprintf(buf, sizeof(buf), "*** FOUND sensor #%d: ID=%08lx P=%dkPa T=%dC",
                             sniffer_count, (unsigned long)id, pressure, temp);
                    uartSendLog(buf);
                    uartBroadcastStatus();
                    if (sniffer_count >= SNIFFER_MAX_SENSORS || sniffer_count >= TPMS_NUM_SENSORS) {
                        uartSendLog("[SNIFFER] All sensors found, stopping");
                        sniffer_stop();
                        return;
                    }
                }
                acc_len = 0;
            }
        }
    }

    if (acc_len >= sizeof(acc_buf) - 64) {
        uint16_t keep = 64;
        memmove(acc_buf, acc_buf + (acc_len - keep), keep);
        acc_len = keep;
    }

    if (sniffer_count > 0 && (millis() - sniffer_discovery_ms) > 60000) {
        uartSendLog("[SNIFFER] No new sensors for 60s, stopping");
        sniffer_stop();
    }
}

// ============================================================================
// Config
// ============================================================================
void init_config() {
    config.magic = EEPROM_MAGIC;
    config.datarate = TPMS_DEFAULT_DATARATE;
    config.deviation = (uint16_t)(TPMS_DEFAULT_DEVIATION * 10);
    config.power = TPMS_DEFAULT_POWER;
    config.tx_enabled = 1;
    config.freq = 0;
    memset(config.reserved, 0, 3);
    config.autoTxInterval = 300;
    config.autoTxPackets = 2;
    config.reserved2 = 0;
    config.battMah = BATT_DEFAULT_MAH;
    ee_buf[EE_LICENSE] = 0;

    for (uint8_t i = 0; i < TPMS_NUM_SENSORS; i++) {
        config.sensors[i].sensor_id = DEFAULT_SENSOR_IDS[i];
        config.sensors[i].pressure_kpa = 230;
        config.sensors[i].temperature_c = 20;
        config.sensors[i].battery_ok = 0;
        config.sensors[i].flags = 0x01;
    }
}

void load_config() {
    eeprom_load();
    if (ee_buf[EE_MAGIC] != EEPROM_MAGIC) {
        init_config();
        save_config();
    } else {
        config.datarate       = ee_read32(EE_DATARATE);
        config.deviation      = ee_read16(EE_DEVIATION);
        config.power          = constrain(ee_buf[EE_POWER], TPMS_POWER_MIN, TPMS_POWER_MAX);
        config.manchester_en  = ee_buf[EE_MANCH_EN];
        config.tx_enabled     = ee_buf[EE_TX_ENABLED];
        config.freq           = constrain(ee_buf[EE_FREQ], 0, 1);
        config.autoTxInterval = ee_read16(EE_TX_INTERVAL);
        config.autoTxPackets  = ee_buf[EE_TX_PACKETS];

        autoTxEnabled  = config.tx_enabled ? true : false;
        autoTxInterval = config.autoTxInterval;
        autoTxPackets  = config.autoTxPackets;

        uint16_t bmah = ee_read16(EE_BATT_MAH);
        config.battMah = (bmah == 0xFFFF || bmah == 0) ? BATT_DEFAULT_MAH : bmah;

        for (uint8_t i = 0; i < TPMS_NUM_SENSORS; i++) {
            int off = EE_SENSORS + i * 7;
            config.sensors[i].sensor_id     = ee_read32(off);
            config.sensors[i].pressure_kpa  = ee_buf[off + 4];
            config.sensors[i].temperature_c = (int8_t)ee_buf[off + 5];
            config.sensors[i].battery_ok    = ee_buf[off + 6] & 0x01;
            config.sensors[i].flags         = ee_buf[off + 6] >> 1;
        }
        for (uint8_t i = 0; i < TPMS_NUM_SENSORS; i++) {
            if (config.sensors[i].flags == 0) config.sensors[i].flags = 0x01;
        }
    }
}

void save_config() {
    flush_trial();
    ee_buf[EE_MAGIC]       = EEPROM_MAGIC;
    ee_write32(EE_DATARATE, config.datarate);
    ee_write16(EE_DEVIATION, config.deviation);
    ee_buf[EE_POWER]       = config.power;
    ee_buf[EE_MANCH_EN]    = config.manchester_en;
    ee_buf[EE_TX_ENABLED]  = config.tx_enabled;
    ee_buf[EE_FREQ]        = config.freq;
    ee_write16(EE_TX_INTERVAL, config.autoTxInterval);
    ee_buf[EE_TX_PACKETS]  = config.autoTxPackets;
    ee_write16(EE_BATT_MAH, config.battMah);

    for (uint8_t i = 0; i < TPMS_NUM_SENSORS; i++) {
        int off = EE_SENSORS + i * 7;
        ee_write32(off, config.sensors[i].sensor_id);
        ee_buf[off + 4] = config.sensors[i].pressure_kpa;
        ee_buf[off + 5] = (uint8_t)config.sensors[i].temperature_c;
        ee_buf[off + 6] = (config.sensors[i].battery_ok & 0x01) | (config.sensors[i].flags << 1);
    }

    if (ee_buf[EE_LICENSE] != 0xFF) ee_buf[EE_LICENSE] = 0;
    if (ee_read32(EE_TRIAL_SEC) == 0xFFFFFFFF) ee_write32(EE_TRIAL_SEC, 0);

    eeprom_commit();
}

// ============================================================================
// Auto-TX
// ============================================================================
void runAutoTx() {
    if (!autoTxEnabled || autoTxRunning || snifferActive) return;
    if (!is_licensed() && is_trial_expired()) return;
    if (isBatteryCritical()) return;

    uint32_t elapsed = millis() - autoTxLastMs;
    if (elapsed < (uint32_t)autoTxInterval * 1000UL) return;

    autoTxRunning = true;
    uartSendLog("[AUTO-TX] Sending burst...");
    send_burst_all();
    uartSendLog("[AUTO-TX] Burst done");
    autoTxRunning = false;
    autoTxLastMs = millis();
    uartBroadcastStatus();
}

// ============================================================================
// JSON Helpers
// ============================================================================
static String jsonGetString(const String &json, const char *key) {
    String search = String("\"") + key + String("\":");
    int pos = json.indexOf(search);
    if (pos < 0) return "";
    int valStart = pos + search.length();
    while (valStart < (int)json.length() && json[valStart] == ' ') valStart++;
    if (valStart >= (int)json.length()) return "";
    if (json[valStart] == '"') {
        int end = json.indexOf('"', valStart + 1);
        if (end < 0) return "";
        return json.substring(valStart + 1, end);
    } else {
        int end = valStart;
        while (end < (int)json.length() &&
               json[end] != ',' && json[end] != '}' && json[end] != ' ' && json[end] != '\n') end++;
        return json.substring(valStart, end);
    }
}

static long jsonGetLong(const String &json, const char *key) {
    String val = jsonGetString(json, key);
    if (val.length() == 0) return 0;
    if (val == "true") return 1;
    if (val == "false") return 0;
    return val.toInt();
}

static String buildStatusJson() {
    String json = "{\"t\":\"status\",\"data\":{";
    json += String("\"sensors\":[");
    for (uint8_t i = 0; i < TPMS_NUM_SENSORS; i++) {
        SensorConfig *s = &config.sensors[i];
        if (i > 0) json += ",";
        bool en = s->flags & 0x01;
        char idBuf[9];
        snprintf(idBuf, sizeof(idBuf), "%08lx", (unsigned long)s->sensor_id);
        uartSendLog("[DBG] status sensor " + String(i) + " id=" + String(idBuf));
        json += "{\"id\":\"" + String(idBuf) + "\",\"pressure\":" + String(s->pressure_kpa)
             + ",\"temp\":" + String(s->temperature_c) + ",\"enabled\":" + (en ? "true" : "false") + "}";
    }
    json += "],\"freq\":" + String(config.freq == 1 ? 433 : 315);
    json += ",\"settings\":{\"datarate\":" + String(config.datarate)
         + ",\"deviation\":" + String(config.deviation / 10.0f, 1)
         + ",\"power\":" + String(config.power) + "}";
    json += ",\"autoTx\":{\"enabled\":" + String(autoTxEnabled ? "true" : "false")
         + ",\"interval\":" + String(autoTxInterval)
         + ",\"packets\":" + String(autoTxPackets) + "}";
    json += ",\"platform\":\"nrf52840\"";
    json += ",\"license\":{\"licensed\":" + String(is_licensed() ? "true" : "false")
         + ",\"trialSec\":" + String(get_trial_seconds())
         + ",\"trialMax\":86400}";
    json += ",\"battery\":{\"mv\":" + String(readBatteryMv())
         + ",\"pct\":" + String(getBatteryPercent())
         + ",\"low\":" + String(isBatteryLow() ? "true" : "false")
         + ",\"critical\":" + String(isBatteryCritical() ? "true" : "false")
         + ",\"mah\":" + String(config.battMah) + "}";
    json += ",\"sniffer\":{\"active\":" + String(snifferActive ? "true" : "false")
         + ",\"count\":" + String(sniffer_count) + ",\"sensors\":[";
    for (uint8_t i = 0; i < sniffer_count; i++) {
        if (i > 0) json += ",";
        char idBuf[9];
        snprintf(idBuf, sizeof(idBuf), "%08lx", (unsigned long)sniffer_sensors[i].sensor_id);
        json += "{\"id\":\"" + String(idBuf) + "\",\"p\":" + String(sniffer_sensors[i].pressure_kpa)
             + ",\"t\":" + String(sniffer_sensors[i].temperature_c) + "}";
    }
    json += "]";

    File df = InternalFS.open("tpms_cfg", FILE_O_READ);
    if (df) {
        uint8_t tmp[EE_SENSORS+4];
        df.read(tmp, sizeof(tmp));
        df.close();
        json += ",\"debug\":{\"magic\":\"0x" + String(tmp[EE_MAGIC], HEX) + "\",\"id0\":\"0x" + String(tmp[EE_SENSORS+3], HEX) + String(tmp[EE_SENSORS+2], HEX) + String(tmp[EE_SENSORS+1], HEX) + String(tmp[EE_SENSORS], HEX) + "\",\"last\":\"" + String(dbg_last_id) + "\"}";
    } else {
        json += ",\"debug\":{\"error\":\"no file\"}";
    }

    json += "}}}";
    return json;
}

static void uartPrint(const String &msg) {
    // Send message in chunks and yield to BLE stack to avoid FIFO loss
    const size_t chunkSize = 20;
    const char *data = msg.c_str();
    size_t len = msg.length();
    size_t sent = 0;
    while (sent < len) {
        size_t n = min(chunkSize, len - sent);
        size_t written = bleUart.write((const uint8_t *)(data + sent), n);
        if (written == 0) {
            delay(1);
            continue;
        }
        sent += written;
        delay(1);
    }
}

void uartSendLog(const String &msg) {
    String json = "{\"t\":\"log\",\"m\":\"";
    String escaped = msg;
    escaped.replace("\\", "\\\\");
    escaped.replace("\"", "\\\"");
    escaped.replace("\n", "\\n");
    json += escaped;
    json += "\"}";
    uartPrint(json);
    uartPrint("\n");
}

void uartBroadcastStatus() {
    String json = buildStatusJson();
    uartPrint(json);
    uartPrint("\n");
}

// ============================================================================
// BLE UART Command Processor
// ============================================================================
void processCommand(const String &msg) {
    uartSendLog("[DBG] MSG(len=" + String(msg.length()) + ")");
    String cmd = jsonGetString(msg, "cmd");
    cmd.trim();

    if (cmd == "status") {
        uartBroadcastStatus();
    } else if (cmd == "burst") {
        uartSendLog("[CMD] Burst all enabled sensors");
        send_burst_all();
        uartSendLog("[CMD] Burst done");
        uartBroadcastStatus();
    } else if (cmd == "tx") {
        autoTxEnabled = true;
        config.tx_enabled = 1;
        autoTxLastMs = millis() - (uint32_t)autoTxInterval * 1000UL;
        save_config();
        uartSendLog("[CMD] Auto-TX enabled");
        uartBroadcastStatus();
    } else if (cmd == "stop") {
        autoTxEnabled = false;
        config.tx_enabled = 0;
        save_config();
        uartSendLog("[CMD] Auto-TX disabled");
        uartBroadcastStatus();
    } else if (cmd == "reset") {
        init_config();
        save_config();
        uartSendLog("[CMD] Config reset to defaults");
        uartBroadcastStatus();
    } else if (cmd == "license") {
        String key = jsonGetString(msg, "key");
        if (key.length() == 8) {
            bool valid = true;
            for (int i = 0; i < 8; i++) {
                char c = key[i];
                if (!isxdigit(c)) { valid = false; break; }
            }
            if (valid) {
                ee_buf[EE_LICENSE] = 0xFF;
                eeprom_commit();
                
                uartSendLog("[LICENSE] Key accepted");
            } else {
                uartSendLog("[LICENSE] Invalid key");
            }
        } else {
            uartSendLog("[LICENSE] Key must be 8 hex chars");
        }
        uartBroadcastStatus();
    } else if (cmd == "sniff_start") {
        sniffer_start();
        uartBroadcastStatus();
    } else if (cmd == "sniff_stop") {
        sniffer_stop();
        uartBroadcastStatus();
    } else if (cmd == "sniff_apply") {
        for (uint8_t i = 0; i < sniffer_count && i < TPMS_NUM_SENSORS; i++) {
            config.sensors[i].sensor_id = sniffer_sensors[i].sensor_id;
            config.sensors[i].pressure_kpa = sniffer_sensors[i].pressure_kpa;
            config.sensors[i].temperature_c = sniffer_sensors[i].temperature_c;
            config.sensors[i].battery_ok = sniffer_sensors[i].battery_ok;
            config.sensors[i].flags = 0x01;
        }
        save_config();
        uartSendLog("[SNIFFER] Applied " + String(sniffer_count) + " sensors");
        uartBroadcastStatus();
    } else if (cmd == "sensor") {
        int sensorIdx = (int)jsonGetLong(msg, "sensor");
        if (sensorIdx >= 0 && sensorIdx < TPMS_NUM_SENSORS) {
            String idStr = jsonGetString(msg, "id");
            if (idStr.length() > 0) {
                bool valid = true;
                for (int i = 0; i < idStr.length() && i < 8; i++) {
                    if (!isxdigit(idStr[i])) { valid = false; break; }
                }
                dbg_last_id[0] = 0;
                snprintf(dbg_last_id, sizeof(dbg_last_id), "id=%s len=%d valid=%s", idStr.c_str(), idStr.length(), (valid && idStr.length() <= 8) ? "Y" : "N");
                uartSendLog("[DBG] idStr='" + idStr + "' len=" + String(idStr.length()) + " valid=" + String(valid ? "Y" : "N"));
            if (valid && idStr.length() <= 8) {
                    config.sensors[sensorIdx].sensor_id = (uint32_t)strtoul(idStr.c_str(), NULL, 16);
                    snprintf(dbg_last_id, sizeof(dbg_last_id), "id=%s parsed=0x%08lx", idStr.c_str(), (unsigned long)config.sensors[sensorIdx].sensor_id);
                    uartSendLog("[DBG] SET sensor_id=0x" + String(config.sensors[sensorIdx].sensor_id, HEX));
                } else {
                    snprintf(dbg_last_id, sizeof(dbg_last_id), "id=%s len=%d REJECTED", idStr.c_str(), idStr.length());
                    uartSendLog("[DBG] NOT setting sensor_id");
                }
            }
            long p = jsonGetLong(msg, "pressure");
            if (msg.indexOf("\"pressure\"") >= 0) config.sensors[sensorIdx].pressure_kpa = (uint8_t)p;
            long t = jsonGetLong(msg, "temp");
            if (msg.indexOf("\"temp\"") >= 0) config.sensors[sensorIdx].temperature_c = (int8_t)t;
            String enabledStr = jsonGetString(msg, "enabled");
            if (enabledStr.length() > 0) {
                if (enabledStr == "true") config.sensors[sensorIdx].flags |= 0x01;
                else config.sensors[sensorIdx].flags &= ~0x01;
            }
            save_config();
            uartSendLog("[API] Sensor " + String(sensorIdx + 1) + " updated");
            uartBroadcastStatus();
        }
    } else if (cmd == "autotx") {
        String enabledStr = jsonGetString(msg, "enabled");
        if (enabledStr.length() > 0) autoTxEnabled = (enabledStr == "true");
        long interval = jsonGetLong(msg, "interval");
        if (msg.indexOf("\"interval\"") >= 0) {
            autoTxInterval = (uint16_t)interval;
            if (autoTxInterval < 5) autoTxInterval = 5;
            if (autoTxInterval > TPMS_MAX_TX_INTERVAL) autoTxInterval = TPMS_MAX_TX_INTERVAL;
        }
        long packets = jsonGetLong(msg, "packets");
        if (msg.indexOf("\"packets\"") >= 0) {
            autoTxPackets = (uint8_t)packets;
            if (autoTxPackets < 1) autoTxPackets = 1;
            if (autoTxPackets > 20) autoTxPackets = 20;
        }
        config.tx_enabled = autoTxEnabled ? 1 : 0;
        config.autoTxInterval = autoTxInterval;
        config.autoTxPackets = autoTxPackets;
        save_config();
        uartSendLog("[API] Auto-TX updated");
        uartBroadcastStatus();
    } else if (cmd == "settings") {
        long freq = jsonGetLong(msg, "freq");
        if (msg.indexOf("\"freq\"") >= 0) {
            config.freq = (freq == 433) ? 1 : 0;
            float f = (config.freq == 1) ? TPMS_FREQ_433 : TPMS_FREQ_315;
            cc1101.setFreq(f);
            cc1101.setFreqConfig(f);
            set_cc1101_power(config.power);
            uartSendLog("[API] Frequency changed to " + String(freq) + " MHz");
        }
        save_config();
        uartBroadcastStatus();
    } else if (cmd.length() == 3 && cmd.startsWith("tx")) {
        uint8_t si = cmd.charAt(2) - '1';
        if (si < TPMS_NUM_SENSORS) {
            uartSendLog("[CMD] TX sensor " + String(si + 1));
            send_pmv107j_sensor(si);
            uartBroadcastStatus();
        }
    } else {
        uartSendLog("[CMD] Unknown: " + cmd);
    }
}
void bleuart_rx_callback(uint16_t conn_handle) {
    (void)conn_handle;

    while (bleUart.available()) {
        char c = bleUart.read();
        if (c == '\n') {
            bleCommandBuffer.trim();
            if (bleCommandBuffer.length() > 0) {
                bleCommandReady = true;
            }
        } else if (!bleCommandReady && bleCommandBuffer.length() < 512) {
            bleCommandBuffer += c;
        }
    }
}

void startAdv() {
    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
    Bluefruit.Advertising.addTxPower();
    Bluefruit.Advertising.addName();
    Bluefruit.Advertising.addService(bleUart);
    Bluefruit.Advertising.restartOnDisconnect(true);
    Bluefruit.Advertising.setInterval(32, 244);
    Bluefruit.Advertising.setFastTimeout(30);
    Bluefruit.Advertising.start(0);
}

// ============================================================================
// Setup / Loop
// ============================================================================
void setup() {
    pinMode(PIN_LED_STATUS, OUTPUT);
    digitalWrite(PIN_LED_STATUS, HIGH);

    pinMode(PIN_CC1101_POWER, OUTPUT);
    digitalWrite(PIN_CC1101_POWER, LOW);

    // BLE init FIRST — needed by InternalFS flash operations (SoftDevice event callback)
    Bluefruit.begin();
    Bluefruit.setName("TPMS-NRF52840");
    Bluefruit.setTxPower(4);
    Bluefruit.Periph.begin();

    // InternalFS init SECOND — may format flash on first boot, uses SoftDevice flash API
    InternalFS.begin();

    bleUart.begin();
    bleUart.setRxCallback(bleuart_rx_callback);

    // Load config from InternalFS
    load_config();

    autoTxEnabled = true;
    config.tx_enabled = 1;
    if (autoTxInterval < 5) autoTxInterval = 5;

    // CC1101
    cc1101_power_on();
    init_cc1101();

    autoTxLastMs = millis();

    // Start advertising LAST — after all init is done
    startAdv();

    digitalWrite(PIN_LED_STATUS, LOW);
    uartSendLog("[SETUP] TPMS nRF52840 v2 ready");
}

void loop() {
    sniffer_loop();
    runAutoTx();

    // Process deferred BLE command outside interrupt context
    if (bleCommandReady) {
        bleCommandReady = false;
        String cmd = bleCommandBuffer;
        bleCommandBuffer = "";
        processCommand(cmd);
    }

    // LED blink heartbeat
    static uint32_t lastLed = 0;
    if (millis() - lastLed > 1000) {
        lastLed = millis();
        digitalWrite(PIN_LED_STATUS, !digitalRead(PIN_LED_STATUS));
    }

    // Trial time tracking
    static uint32_t lastTrialUpdate = 0;
    if (!is_licensed()) {
        uint32_t now = millis();
        if (now - lastTrialUpdate >= 30000) {
            uint32_t elapsed = (now - lastTrialUpdate) / 1000;
            if (elapsed > 0) add_trial_seconds(elapsed);
            lastTrialUpdate = now;
        }
        if (is_trial_expired()) {
            if (snifferActive) sniffer_stop();
        }
    }

    delay(1);
}
