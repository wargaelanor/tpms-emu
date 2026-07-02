/**
 * TPMS Emulator for Acura RDX 2008 - v5 (ESP32-C3 Port)
 *
 * v5 changes: ported from ESP8266 D1 Mini to ESP32-C3 Mini
 *   - New pin mapping for ESP32-C3 FSPI bus
 *   - ESP32 deep sleep API (esp_sleep)
 *   - Removed yield() calls (not needed on ESP32 RTOS)
 *   - Removed Serial debug output from all code
 *   - ~7uA deep sleep current (vs ~20uA on ESP8266)
 *
 * Platform: ESP32-C3 Mini + CC1101
 * Frequency: 315 MHz (North America TPMS band)
 * Protocol: PMV-107J (Pacific Industrial) - rtl_433 protocol 110
 *
 * Encoding:
 *   RF: 2-FSK (GFSK), deviation ~38 kHz, 10 kbaud
 *   Physical: NRZ (mark=1, space=0, 100us per bit)
 *   Line code: Differential Manchester Encoding
 *   Preamble: 16 zeros + "111110" (6 bits)
 *   Payload: 66 bits after DM decode
 *     [28-bit ID][1-bit bat][2-bit cnt][1-bit 0][1-bit rapid][1-bit fail]
 *     [8-bit P][8-bit !P][8-bit T][8-bit CRC]
 *   CRC-8: poly=0x13, init=0x00, over first 58 bits
 *
 * Wiring (ESP32-C3 Mini -> CC1101 8-pin module):
 *   GPIO6 (Power Ctrl) - CC1101 Pin 1 (VCC)  <-- direct power from GPIO!
 *   GPIO1 (FSPI MOSI)  - CC1101 Pin 3 (SI/DIN)
 *   GPIO3 (FSPI MISO)  - CC1101 Pin 4 (SO/DOUT)
 *   GPIO2 (FSPI SCK)   - CC1101 Pin 5 (CLK)
 *   GPIO7 (CSN)         - CC1101 Pin 6 (CSN)
 *   GPIO4 (GDO0)        - CC1101 Pin 7 (GDO0)
 *   GPIO5 (GDO2)        - CC1101 Pin 8 (GDO2)
 *   GND                  - CC1101 Pin 2 (GND)
 *
 * IMPORTANT: CC1101 VCC powered directly from GPIO6!
 *   - GPIO HIGH (3.3V) = CC1101 ON
 *   - GPIO LOW  (0V)   = CC1101 OFF (deep sleep)
 *   - CC1101 TX at PA5 draws ~15-20mA, ESP32-C3 GPIO handles up to 40mA peak
 *   - SPI lines are set LOW before power-off to prevent latch-up through
 *     CC1101 input protection diodes (SPI.end + manual pin control)
 */

#include <Arduino.h>
#include <SPI.h>
#include <EEPROM.h>
#include <string.h>
#include <DNSServer.h>
#include <WebSocketsServer.h>
#include "CC1101.h"

#if defined(ESP8266)
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Updater.h>
#define WebServer ESP8266WebServer
// No mbedTLS on ESP8266 — license always active
#else
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <esp_sleep.h>
#include <mbedtls/md.h>
#endif

// ============================================================================
// Pin Definitions (platform-specific)
// ============================================================================
#if defined(ESP8266)
// ESP8266 D1 Mini:
//   D5 (GPIO14) — SCK   |  D6 (GPIO12) — MOSI  |  D7 (GPIO13) — MISO
//   D8 (GPIO15) — CSN   |  D1 (GPIO5)  — GDO0  |  D2 (GPIO4)  — GDO2
//   D4 (GPIO2)  — Power (N-MOSFET)     |  D0 (GPIO16) — RST (wake)
#define PIN_CC1101_CS      15  // D8  - CC1101 CSN
#define PIN_CC1101_GDO0    5   // D1  - CC1101 GDO0
#define PIN_CC1101_GDO2    4   // D2  - CC1101 GDO2
#define PIN_CC1101_POWER   2   // D4  - N-MOSFET gate
#undef PIN_SPI_MOSI
#undef PIN_SPI_MISO
#undef PIN_SPI_SCK
#define PIN_SPI_MOSI        12  // D6  - HSPI MOSI (user wiring)
#define PIN_SPI_MISO        13  // D7  - HSPI MISO (user wiring)
#define PIN_SPI_SCK         14  // D5  - HSPI SCK
#else
// ESP32-C3 Mini:
//   GPIO1 — MOSI  |  GPIO3 — MISO  |  GPIO2 — SCK
//   GPIO7 — CSN   |  GPIO4 — GDO0  |  GPIO5 — GDO2
//   GPIO6 — VCC (direct power)
#define PIN_CC1101_CS      7   // GPIO7  - CC1101 CSN
#define PIN_CC1101_GDO0    4   // GPIO4  - CC1101 GDO0
#define PIN_CC1101_GDO2    5   // GPIO5  - CC1101 GDO2
#define PIN_CC1101_POWER   6   // GPIO6  - CC1101 VCC (direct power)
#define PIN_SPI_MOSI        1   // GPIO1  - FSPI MOSI
#define PIN_SPI_MISO        3   // GPIO3  - FSPI MISO
#define PIN_SPI_SCK         2   // GPIO2  - FSPI SCK
#endif

// ============================================================================
// Configuration Constants
// ============================================================================
#define TPMS_FREQ_315            315.0
#define TPMS_FREQ_433            433.92
#define TPMS_FREQ_DEFAULT        TPMS_FREQ_315
#define TPMS_DEFAULT_DATARATE    10000
#define TPMS_DEFAULT_DEVIATION   38.0
#define TPMS_DEFAULT_POWER       5    // PA index 5 (5 dBm), range 1-5
#define TPMS_TX_INTERVAL_MS      200  // ms between packets in burst
#define TPMS_POWER_MIN           1
#define TPMS_POWER_MAX           5
#define TPMS_BURST_COUNT         5
#define TPMS_MAX_TX_INTERVAL      900   // max auto-TX interval (seconds)
#define WIFI_CONFIG_TIMEOUT_MS     180000  // 3 minutes WiFi AP
#define TPMS_BURST_DELAY_MS      200
#define TPMS_NUM_SENSORS         4
#define EEPROM_MAGIC             0xAF  // v7.2: reset EEPROM for new defaults

// Battery voltage thresholds (mV)
#define BATT_FULL_MV             4200
#define BATT_LOW_MV              3300
#define BATT_CRITICAL_MV         3000
#define BATT_DEFAULT_MAH         3000

// ============================================================================
// EEPROM byte layout (49 bytes total)
// ============================================================================
#define EE_MAGIC       0    // 1 byte
#define EE_DATARATE    1    // 4 bytes (uint32_t LE)
#define EE_DEVIATION   5    // 2 bytes (uint16_t LE, stored as khz*10)
#define EE_POWER       7    // 1 byte (PA index 1-5)
#define EE_MANCH_EN    8    // 1 byte (unused, always 0)
#define EE_TX_ENABLED  9    // 1 byte (global auto-TX enable)
#define EE_TX_INTERVAL 10   // 2 bytes (uint16_t LE, seconds)
#define EE_TX_PACKETS  12   // 1 byte (packets per sensor per burst)
#define EE_FREQ        13   // 1 byte (0=315MHz, 1=433MHz)
#define EE_SENSORS     14   // 4 sensors x 7 bytes = 28 bytes
#define EE_LICENSE     42   // 1 byte: 0=unactivated/trial, 0xFF=licensed
#define EE_TRIAL_SEC   43   // 4 bytes (uint32_t LE): accumulated seconds of operation (max 86400)
#define EE_BATT_MAH    47   // 2 bytes (uint16_t LE): battery capacity in mAh
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

// ============================================================================
// License System (software-based, no Flash Encryption)
// ============================================================================
// Device ID = WiFi MAC address (12 hex chars)
// License Key = HMAC-SHA256(secret, device_id) → first 4 bytes → 8 hex chars
// Stored in EEPROM[EE_LICENSE]: 0 = unlicensed, 0xFF = licensed
// ESP8266: simple format check (no mbedTLS); ESP32-C3: full HMAC verification
#if defined(ESP8266)

static String get_device_id() {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char buf[13];
    snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buf);
}
static bool verify_license(const char *device_id, const char *license_key) {
    // ESP8266: accept any 8-char hex key (no HMAC — mbedTLS unavailable)
    if (strlen(license_key) != 8) return false;
    for (int i = 0; i < 8; i++) {
        char c = license_key[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')))
            return false;
    }
    return true;
}

#else

const uint8_t LICENSE_SECRET[16] = {
    0x2A, 0x7C, 0xB9, 0x41, 0xD3, 0xE5, 0x8F, 0x06,
    0x9C, 0x15, 0xF7, 0xBA, 0x40, 0x62, 0x83, 0x1E
};

static String get_device_id() {
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    char buf[13];
    snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buf);
}

static void hmac_sha256(const uint8_t *key, size_t keylen,
                        const uint8_t *data, size_t datalen,
                        uint8_t *output) {
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    mbedtls_md_hmac_starts(&ctx, key, keylen);
    mbedtls_md_hmac_update(&ctx, data, datalen);
    mbedtls_md_hmac_finish(&ctx, output);
    mbedtls_md_free(&ctx);
}

static bool verify_license(const char *device_id, const char *license_key) {
    uint8_t hmac[32];
    hmac_sha256(LICENSE_SECRET, sizeof(LICENSE_SECRET),
                (const uint8_t *)device_id, strlen(device_id), hmac);
    char expected[9];
    snprintf(expected, sizeof(expected), "%02X%02X%02X%02X",
             hmac[0], hmac[1], hmac[2], hmac[3]);
    return strcmp(expected, license_key) == 0;
}

#endif

static bool is_licensed() {
    return ee_buf[EE_LICENSE] == 0xFF;
}

static uint32_t trial_unwritten = 0;  // seconds accumulated but not yet written to EEPROM

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
    for (int i = 0; i < 4; i++) EEPROM.write(EE_TRIAL_SEC + i, ee_buf[EE_TRIAL_SEC + i]);
    EEPROM.commit();
    trial_unwritten = 0;
}

static void add_trial_seconds(uint32_t s) {
    trial_unwritten += s;
    uint32_t total = get_trial_seconds();
    // Batch writes: every 60s or on expiry
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

#ifndef ESP8266
static void mark_licensed(uint8_t *buf) {
    buf[EE_LICENSE] = 0xFF;
    EEPROM.write(EE_LICENSE, 0xFF);
    EEPROM.commit();
}
#endif

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
    uint8_t  battery_ok;   // always 0 (battery OK) for protocol
    uint8_t  flags;        // bit 0 = enabled for auto-TX (1=on, 0=off)
} __attribute__((packed));

struct SystemConfig {
    uint8_t  magic;
    uint32_t datarate;
    uint16_t deviation;     // stored as kHz * 10
    uint8_t  power;         // PA index 1-5
    uint8_t  manchester_en; // always 0
    uint8_t  tx_enabled;    // global auto-TX on/off
    uint8_t  freq;          // 0=315MHz, 1=433MHz
    uint8_t  reserved[3];
    uint16_t autoTxInterval;
    uint8_t  autoTxPackets;
    uint8_t  reserved2;
    SensorConfig sensors[TPMS_NUM_SENSORS];
    uint16_t battMah;       // battery capacity in mAh (default 3000)
} __attribute__((packed));

// ============================================================================
// WiFi / Web Server / WebSocket
// ============================================================================
#if defined(ESP8266)
#define AP_SSID     "TPMS"
#else
#define AP_SSID     "TPMS"
#endif
#define AP_PASS     "12345678"
#define DNS_PORT    53

WebServer server(80);
DNSServer dnsServer;
WebSocketsServer wsServer(81, "/ws");
const IPAddress apIP(192, 168, 4, 1);

// ============================================================================
// Auto-TX Configuration
// ============================================================================
bool     autoTxEnabled = true;
uint16_t autoTxInterval = 60;   // seconds between bursts
uint8_t  autoTxPackets  = 2;    // packets per sensor per burst
uint32_t autoTxLastMs   = 0;
bool     autoTxRunning  = false;

// ============================================================================
// Sniffer Mode
// ============================================================================
bool     snifferActive  = false;
uint32_t snifferStartMs = 0;
uint32_t g_lastDiagTx   = 0;  // global for DIAG-TX timer

#define  SNIFFER_TIMEOUT_MS 3600000  // 1 hour sniffing
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
uint32_t sniffer_discovery_ms = 0;  // when last NEW sensor was found

// ============================================================================
// Global Variables
// ============================================================================
CC1101 cc1101(PIN_CC1101_CS, PIN_CC1101_GDO0, PIN_CC1101_GDO2);
SystemConfig config;
uint32_t last_tx_time = 0;
uint8_t  tx_counter = 0;
uint8_t  burst_count = 0;

// ============================================================================
// WiFi Timeout & Power Management
// ============================================================================
bool     wifiActive     = false;
uint32_t wifiTimeoutMs  = 0;
uint8_t  wsClients      = 0;
#define CC1101_WAKE_DELAY_MS 2   // CC1101 power-on stabilization (datasheet min: 150us)

// ============================================================================
// Default sensor IDs for Acura RDX 2008
// ============================================================================
const uint32_t DEFAULT_SENSOR_IDS[TPMS_NUM_SENSORS] = {
    0x00298088, 0x0466E088, 0x0D784088, 0x0C765088
};

// Sensor position labels (for web UI)
const char* SENSOR_LABELS[TPMS_NUM_SENSORS] = {
    "PL", "PP", "ZL", "ZP"
};

// ============================================================================
// Function Prototypes
// ============================================================================
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
void setupWiFi();
void setupWebServer();
void setupWebSocket();
void cc1101_power_on();
void cc1101_power_off();
void send_burst_all();
void enterDeepSleep();
void handleRoot();
void wsSendLog(const String &msg);
void handleApiStatus();
void handleApiSensor();
void handleApiSettings();
void handleApiAutoTx();
void handleApiCmd();
void handleApiUpdate();
void sendStatusToWs(uint8_t clientNum);
void wsBroadcastStatus();
void wsSendPacketDecode(uint8_t sensorIdx, uint32_t id, uint8_t pByte, uint8_t tByte, uint8_t cnt, uint8_t crc);
static String buildStatusJson();
void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length);
void runAutoTx();
static String jsonGetString(const String &json, const char *key);
static long jsonGetLong(const String &json, const char *key);

// Bit manipulation helpers
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

// ============================================================================
// CRC-8 Calculation (PMV-107J: poly=0x13, init=0x00)
// ============================================================================
uint8_t calculate_crc8_pmv(const uint8_t *data, uint8_t len) {
    uint8_t crc = 0x00;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ 0x13;
            } else {
                crc = crc << 1;
            }
        }
    }
    return crc;
}

// ============================================================================
// Build 66-bit PMV-107J payload as bit array
// ============================================================================
uint16_t build_pmv107j_payload(uint8_t sensor_idx, uint8_t *bit_buf) {
    if (sensor_idx >= TPMS_NUM_SENSORS) return 0;
    SensorConfig *s = &config.sensors[sensor_idx];
    memset(bit_buf, 0, 16);
    uint16_t pos = 0;

    // 28-bit Sensor ID (MSB first)
    uint32_t id = s->sensor_id;
    for (int8_t i = 27; i >= 0; i--) {
        set_bit(bit_buf, pos++, (id >> i) & 1);
    }

    // bit 28: Battery low (always 0 = OK)
    set_bit(bit_buf, pos++, 0);

    // bits 29-30: Counter (2 bits, cycles 1,2,3)
    uint8_t cnt = (tx_counter % 3) + 1;
    set_bit(bit_buf, pos++, (cnt >> 1) & 1);
    set_bit(bit_buf, pos++, cnt & 1);

    // bits 31-33: Must be 0
    set_bit(bit_buf, pos++, 0);
    set_bit(bit_buf, pos++, 0);
    set_bit(bit_buf, pos++, 0);

    // bits 34-41: Pressure (kPa / 2.48 + 40)
    uint8_t pressure_byte = (uint8_t)((float)s->pressure_kpa / PMV107J_PRESSURE_KPA_SCALE + PMV107J_PRESSURE_OFFSET);
    for (int8_t i = 7; i >= 0; i--) {
        set_bit(bit_buf, pos++, (pressure_byte >> i) & 1);
    }

    // bits 42-49: Inverted pressure
    uint8_t inv_pressure = pressure_byte ^ 0xFF;
    for (int8_t i = 7; i >= 0; i--) {
        set_bit(bit_buf, pos++, (inv_pressure >> i) & 1);
    }

    // bits 50-57: Temperature (Celsius + 40)
    uint8_t temp_byte = (uint8_t)(s->temperature_c + PMV107J_TEMP_OFFSET);
    for (int8_t i = 7; i >= 0; i--) {
        set_bit(bit_buf, pos++, (temp_byte >> i) & 1);
    }

    // CRC-8 over first 58 bits (with 6 zero padding for byte alignment)
    uint8_t crc_buf[8];
    memset(crc_buf, 0, 8);
    for (uint16_t i = 0; i < 58; i++) {
        set_bit(crc_buf, 6 + i, get_bit(bit_buf, i));
    }
    uint8_t crc = calculate_crc8_pmv(crc_buf, 8);

    // bits 58-65: CRC
    for (int8_t i = 7; i >= 0; i--) {
        set_bit(bit_buf, pos++, (crc >> i) & 1);
    }

    tx_counter = (tx_counter + 1) & 0xFF;

    wsSendPacketDecode(sensor_idx, id, pressure_byte, temp_byte, cnt, crc);
    return pos;
}

// ============================================================================
// Differential Manchester Encoder
// ============================================================================
uint16_t differential_manchester_encode(const uint8_t *data_bits, uint16_t num_bits,
                                         uint8_t *out_buf) {
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

// ============================================================================
// Send Complete PMV-107J Packet
// ============================================================================
void send_pmv107j_sensor(uint8_t sensor_idx) {
    if (sensor_idx >= TPMS_NUM_SENSORS) return;

    // Step 1: Build 66-bit payload
    uint8_t payload_bits[16];
    uint16_t payload_len = build_pmv107j_payload(sensor_idx, payload_bits);
    if (payload_len != PMV107J_TOTAL_BITS) return;

    // Step 2: Build data to DM encode: '1' + 66 bits + '1' = 68 bits
    uint8_t dm_input[16];
    memset(dm_input, 0, 16);
    uint16_t dm_in_pos = 0;
    set_bit(dm_input, dm_in_pos++, 1);
    for (uint16_t i = 0; i < payload_len; i++) {
        set_bit(dm_input, dm_in_pos++, get_bit(payload_bits, i));
    }
    set_bit(dm_input, dm_in_pos++, 1);

    // Step 3: Differential Manchester encode
    uint8_t dm_output[40];
    uint16_t dm_out_len = differential_manchester_encode(dm_input, dm_in_pos, dm_output);

    // Step 4: Build complete on-air bitstream
    // 16 zeros + 111110 + DM data + 000000
    uint8_t tx_buf[30];
    memset(tx_buf, 0, 30);
    uint16_t tx_pos = 0;

    tx_pos = PMV107J_SETTLE_BITS;
    for (uint8_t i = 0; i < PMV107J_PREAMBLE_BITS; i++) {
        if (i < 5) set_bit(tx_buf, tx_pos + i, 1);
    }
    tx_pos += PMV107J_PREAMBLE_BITS;

    for (uint16_t i = 0; i < dm_out_len; i++) {
        set_bit(tx_buf, tx_pos + i, get_bit(dm_output, i));
    }
    tx_pos += dm_out_len;
    tx_pos += PMV107J_TRAILER_BITS;

    uint8_t tx_bytes = (tx_pos + 7) / 8;
    cc1101_send_raw(tx_buf, tx_bytes);
}

// ============================================================================
// CC1101 Configuration Functions
// ============================================================================
bool set_cc1101_data_rate(uint32_t baud) {
    if (baud < 600 || baud > 500000) return false;
    cc1101.setMHZOsc(26.0f);
    bool result = cc1101.setDRate(baud);
    if (result) config.datarate = baud;
    return result;
}

bool set_cc1101_deviation(float khz) {
    if (khz < 1.58 || khz > 380.0) return false;
    bool result = cc1101.setDeviation(khz);
    if (result) config.deviation = (uint16_t)(khz * 10);
    return result;
}

void set_cc1101_power(int8_t pa_index) {
    pa_index = constrain(pa_index, TPMS_POWER_MIN, TPMS_POWER_MAX);
    cc1101.setPA(pa_index);
    config.power = pa_index;
}

// ============================================================================
// CC1101 Power Control (direct GPIO — no MOSFET)
// ============================================================================
//
// CC1101 VCC is connected directly to GPIO6.
// GPIO HIGH = 3.3V to CC1101 = ON
// GPIO LOW  = 0V to CC1101 = OFF
//
// CRITICAL: Before cutting power, all SPI lines (MOSI, SCK) must be LOW
// to prevent current flow through CC1101 input protection diodes.
// When CC1101 VCC=0 but SPI inputs are at 3.3V, the ESD diodes on
// CC1101 pins would forward-bias and draw excessive current from the
// ESP32 GPIO pins.
//
// Sequence:
//   Power ON:  GPIO HIGH -> delay 2ms -> SPI.begin() -> init_cc1101()
//   Power OFF: setIdle() -> CS HIGH -> SPI.end() -> pins LOW -> GPIO LOW
//
void cc1101_power_on() {
    digitalWrite(PIN_CC1101_POWER, HIGH);
    delay(CC1101_WAKE_DELAY_MS);
}

void cc1101_power_off() {
    // Put CC1101 in idle to stop any ongoing activity
    cc1101.setIdleState();
    delay(1);

    // Deassert CS (high) before SPI shutdown
    digitalWrite(PIN_CC1101_CS, HIGH);

#if !defined(ESP8266)
    // ESP32-C3: SPI lines must be LOW before cutting VCC to prevent latch-up
    // (CC1101 VCC = GPIO6, SPI inputs at 3.3V would forward-bias ESD diodes)
    SPI.end();
    pinMode(PIN_SPI_MOSI, OUTPUT);
    digitalWrite(PIN_SPI_MOSI, LOW);
    pinMode(PIN_SPI_SCK, OUTPUT);
    digitalWrite(PIN_SPI_SCK, LOW);
    pinMode(PIN_SPI_MISO, INPUT);
#else
    // ESP8266: CC1101 VCC stays on (3.3V), GND switched via N-MOSFET
    // No latch-up risk — just end SPI
    SPI.end();
#endif

    // Cut power to CC1101
    digitalWrite(PIN_CC1101_POWER, LOW);
}

// ============================================================================
// Battery Voltage Monitoring
// ============================================================================
// Both boards use 10kΩ+10kΩ voltage divider: Vbat → 10k → Vout → 10k → GND
// ESP8266 A0: Vout = Vbat/2, ADC 0-1023 (non-linear, needs calibration)
// ESP32-C3 GPIO0: Vout = Vbat/2, analogReadMilliVolts() returns mV on pin

uint16_t readBatteryMv() {
#if defined(ESP8266)
    // ESP8266 ADC: 10-bit (0-1023), reference ~3.3V but A0 max ~1V
    // With 1:2 divider: Vbat = A0 * 3.3V / 1023 * 2 * 1000
    uint32_t raw = analogRead(A0);
    return (uint16_t)(raw * 3300UL * 2 / 1023);
#else
    // ESP32-C3: analogReadMilliVolts() returns millivolts on the pin
    // With 1:2 divider: Vbat = pin_mV * 2
    uint32_t mv = analogReadMilliVolts(0);
    return (uint16_t)(mv * 2);
#endif
}

uint8_t getBatteryPercent() {
    uint16_t mv = readBatteryMv();
    if (mv >= BATT_FULL_MV) return 100;
    if (mv <= BATT_CRITICAL_MV) return 0;
    return (uint8_t)((mv - BATT_CRITICAL_MV) * 100 / (BATT_FULL_MV - BATT_CRITICAL_MV));
}

bool isBatteryLow() {
    return readBatteryMv() < BATT_LOW_MV;
}

bool isBatteryCritical() {
    return readBatteryMv() < BATT_CRITICAL_MV;
}

// ============================================================================
// CC1101 Initialization
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

void set_cc1101_manchester(bool enable) {
    cc1101.setManc(enable ? 1 : 0);
    config.manchester_en = enable ? 1 : 0;
}

// ============================================================================
// Sniffer Mode Functions
// ============================================================================
void sniffer_start() {
    if (!can_sniff()) {
        Serial.println("[SNIFFER] BLOCKED — trial expired, license required");
        wsSendLog("[SNIFFER] Trial expired — license key required. Contact: wargaelanor@yandex.ru");
        return;
    }
    Serial.println("[SNIFFER] Starting...");
    snifferActive = true;
    snifferStartMs = millis();
    sniffer_count = 0;
    memset(sniffer_sensors, 0, sizeof(sniffer_sensors));

    // Reconfigure CC1101 for RX
    float freq = (config.freq == 1) ? TPMS_FREQ_433 : TPMS_FREQ_315;
    cc1101.setIdleState();
    delay(1);
    cc1101.setFreqConfig(freq);
    cc1101.setFreq(freq);
    cc1101.setRxConfig();
    // 2-FSK — MOD_FORMAT=000 at MDMCFG2 bits 6:4
    cc1101.setModulation(0);   // 2-FSK
    cc1101.setSyncMode(0);     // no preamble/sync, no CS required
    // NOTE: PMV-107J uses Differential Manchester (software decode in dm_decode).
    // DO NOT enable HW Manchester decoding (setManc) — it would mangle the DM bits.
    set_cc1101_data_rate(config.datarate);  // match TX data rate
    set_cc1101_deviation(38.0f);
    // Re-calibrate before RX (overwriting FSCAL with hardcoded values invalidated it)
    cc1101.sendCommand(CC1101_SCAL);
    delay(3);
    cc1101.flushRxFifo();
    cc1101.setRxState();

    Serial.printf("[SNIFFER] Freq=%.0f MHz, DR=%dk, Dev=38kHz\n", freq, config.datarate);
    Serial.printf("[SNIFFER] MARCSTATE=0x%02X\n", cc1101.getMarcState());
    Serial.printf("[SNIFFER] RSSI=%d dBm\n", cc1101.getRssi());

    // Dump vital registers to verify configuration
    Serial.printf("[SNIFFER] FREQ2=0x%02X FREQ1=0x%02X FREQ0=0x%02X\n",
                  cc1101.readReg(CC1101_FREQ2),
                  cc1101.readReg(CC1101_FREQ1),
                  cc1101.readReg(CC1101_FREQ0));
    Serial.printf("[SNIFFER] MDMCFG4=0x%02X MDMCFG3=0x%02X MDMCFG2=0x%02X\n",
                  cc1101.readReg(CC1101_MDMCFG4),
                  cc1101.readReg(CC1101_MDMCFG3),
                  cc1101.readReg(CC1101_MDMCFG2));
    Serial.printf("[SNIFFER] PKTCTRL0=0x%02X PKTCTRL1=0x%02X PKTLEN=0x%02X\n",
                  cc1101.readReg(CC1101_PKTCTRL0),
                  cc1101.readReg(CC1101_PKTCTRL1),
                  cc1101.readReg(CC1101_PKTLEN));
    Serial.printf("[SNIFFER] MCSM0=0x%02X MCSM1=0x%02X MCSM2=0x%02X\n",
                  cc1101.readReg(CC1101_MCSM0),
                  cc1101.readReg(CC1101_MCSM1),
                  cc1101.readReg(CC1101_MCSM2));
    Serial.printf("[SNIFFER] DEVIATN=0x%02X FOCCFG=0x%02X AGCCTRL2=0x%02X AGCCTRL1=0x%02X\n",
                  cc1101.readReg(CC1101_DEVIATN),
                  cc1101.readReg(CC1101_FOCCFG),
                  cc1101.readReg(CC1101_AGCCTRL2),
                  cc1101.readReg(CC1101_AGCCTRL1));
    uint8_t freqest = cc1101.readStatusReg(CC1101_FREQEST);
    uint8_t lqi = cc1101.readStatusReg(CC1101_LQI);
    Serial.printf("[SNIFFER] FREQEST=%d LQI=%d\n", (int8_t)freqest, lqi);

    Serial.println("[SNIFFER] Listening for TPMS sensors...");
    wsSendLog("[SNIFFER] Started - listening for TPMS sensors...");

    // Frequency sweep diagnostics: try multiple frequencies to detect crystal mismatch
    // Common CC1101 module crystals: 26, 26.12, 27 MHz (not 26 MHz as assumed)
    struct { float mhz; const char* note; } fCandidates[] = {
        {315.0f,  "26 MHz crystal (correct assumption)"},
        {302.0f,  "27.12 MHz crystal"},
        {303.3f,  "27 MHz crystal"},
        {313.6f,  "26.12 MHz crystal"},
        {314.5f,  "slight offset"},
        {314.8f,  "slight offset"},
        {315.2f,  "slight offset"},
        {315.5f,  "slight offset"},
        {316.0f,  "slight offset"},
        {333.3f,  "24.576 MHz crystal"},
        {341.3f,  "24 MHz crystal"},
    };
    for (int ci = 0; ci < 11; ci++) {
        cc1101.setIdleState();
        delay(1);
        cc1101.setFreq(fCandidates[ci].mhz);
        cc1101.sendCommand(CC1101_SCAL);
        delay(3);
        cc1101.flushRxFifo();
        cc1101.setRxState();
        // Listen for 50 ms to catch a sensor transmission
        delay(50);
        int rssi = cc1101.getRssi();
        uint8_t marc = cc1101.getMarcState();
        uint8_t rxBytes = cc1101.getRxBytes();
        Serial.printf("[DIAG] %6.1f MHz -> RSSI=%3d dBm MARC=0x%02X FIFO=%u  (%s)\n",
                      fCandidates[ci].mhz, rssi, marc, rxBytes, fCandidates[ci].note);
    }
    // Restore original frequency
    cc1101.setIdleState();
    delay(1);
    cc1101.setFreq(freq);
    cc1101.setFreqConfig(freq);
    cc1101.setRxConfig();
    cc1101.setModulation(0);  // 2-FSK
    cc1101.setSyncMode(0);
    set_cc1101_data_rate(config.datarate);
    set_cc1101_deviation(38.0f);
    cc1101.sendCommand(CC1101_SCAL);
    delay(3);
    cc1101.flushRxFifo();
    cc1101.setRxState();
    Serial.println("[DIAG] Frequency sweep complete — restored to normal RX");
}

void sniffer_save_discovered() {
    if (sniffer_count == 0) return;
    for (uint8_t i = 0; i < sniffer_count && i < TPMS_NUM_SENSORS; i++) {
        config.sensors[i].sensor_id = sniffer_sensors[i].sensor_id;
        config.sensors[i].pressure_kpa = sniffer_sensors[i].pressure_kpa;
        config.sensors[i].temperature_c = sniffer_sensors[i].temperature_c;
        config.sensors[i].battery_ok = sniffer_sensors[i].battery_ok;
        config.sensors[i].flags = 0x01;  // enabled
    }
    // Clear unused slots
    for (uint8_t i = sniffer_count; i < TPMS_NUM_SENSORS; i++) {
        config.sensors[i].sensor_id = DEFAULT_SENSOR_IDS[i];
        config.sensors[i].pressure_kpa = 230;
        config.sensors[i].temperature_c = 20;
        config.sensors[i].battery_ok = 0;
        config.sensors[i].flags = 0x01;
    }
    save_config();
}

void sniffer_stop() {
    if (!snifferActive) return;
    snifferActive = false;
    cc1101.setIdleState();
    delay(1);

    char buf[64];
    snprintf(buf, sizeof(buf), "[SNIFFER] Stopped - %d sensors found", sniffer_count);
    Serial.println(buf);
    wsSendLog("[SNIFFER] Stopped - " + String(sniffer_count) + " sensors found");

    if (sniffer_count > 0) {
        sniffer_save_discovered();
        snifferStartMs = 0xFFFFFFFF;  // don't auto-restart
        Serial.println("[SNIFFER] Sensors saved to EEPROM, sniffer will stay off");
    } else {
        snifferStartMs = millis() + 10000;  // retry in 10s
    }
    wsBroadcastStatus();
}

// Differential Manchester Decode

// Differential Manchester Decode
// Input: DM encoded bits, Output: decoded bits
uint16_t dm_decode(const uint8_t *dm_bits, uint16_t dm_len, uint8_t *out_bits) {
    uint16_t out_pos = 0;
    uint8_t prev_level = 0;  // initial NRZ level (last preamble bit = 0)

    for (uint16_t i = 0; i < dm_len && out_pos < 128; i += 2) {
        bool b1 = (dm_bits[i / 8] >> (7 - (i % 8))) & 1;
        bool b2 = (dm_bits[(i + 1) / 8] >> (7 - ((i + 1) % 8))) & 1;

        if (b1 == b2) continue;  // invalid DM (00 or 11), skip

        if (b1 == prev_level) {
            // same start level → data bit = 1 (no start transition)
            out_bits[out_pos / 8] |= (1 << (7 - (out_pos % 8)));
        } else {
            // different start level → data bit = 0 (start transition)
            out_bits[out_pos / 8] &= ~(1 << (7 - (out_pos % 8)));
        }
        out_pos++;
        prev_level = b2;  // end level of this bit = start for next
    }
    return out_pos;
}

// Decode PMV-107J packet from raw CC1101 RX data
bool sniffer_decode_packet(const uint8_t *raw, uint8_t len, uint32_t *id, uint8_t *pressure, int8_t *temp) {
    if (len < 5) return false;

    // Find preamble: 16 zeros + 111110
    // Search from END backwards — last valid preamble is most likely the real signal
    // (noise before signal contains false preambles)
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
            // Need at least 136 bits (68 DM pairs * 2) after preamble
            uint16_t bits_after = (len * 8) - (i + 6);
            if (bits_after >= 136) {
                preamble_end = i + 6;
                found = true;
                break;
            }
        }
    }

    if (!found) {
        Serial.printf("[DECODE] Preamble not found in %d bytes\n", len);
        return false;
    }

    // DM decode the payload after preamble
    uint8_t dm_data[80];
    memset(dm_data, 0, sizeof(dm_data));

    // Copy raw bits after preamble to dm_data (68 DM bits = 136 NRZ + 6 trailer = 142)
    uint16_t dm_bit_count = 0;
    uint16_t max_bits = (len * 8) - preamble_end;
    if (max_bits > 136) max_bits = 136;
    for (uint16_t i = preamble_end; i < preamble_end + max_bits; i++) {
        uint8_t byte_idx = i / 8;
        uint8_t bit_idx = 7 - (i % 8);
        bool bit = (raw[byte_idx] >> bit_idx) & 1;
        if (bit) {
            dm_data[dm_bit_count / 8] |= (1 << (7 - (dm_bit_count % 8)));
        }
        dm_bit_count++;
    }

    // DM decode
    uint8_t decoded[40];
    memset(decoded, 0, sizeof(decoded));
    uint16_t decoded_len = dm_decode(dm_data, dm_bit_count, decoded);

    if (decoded_len < 68) {
        Serial.printf("[DECODE] DM decode too short: %d bits (need 68)\n", decoded_len);
        return false;
    }

    // NOTE: TX encodes '1' + 66 payload bits + '1' = 68 DM bits
    // Skip first bit (start=1), payload is bits 1-66
    #define DECODE_OFFSET 1

    // Extract 28-bit ID (payload bits 0-27)
    *id = 0;
    for (int8_t i = 0; i < 28; i++) {
        uint8_t byte_idx = (DECODE_OFFSET + i) / 8;
        uint8_t bit_idx = 7 - ((DECODE_OFFSET + i) % 8);
        bool bit = (decoded[byte_idx] >> bit_idx) & 1;
        *id = (*id << 1) | bit;
    }

    // Extract pressure (payload bits 34-41)
    uint8_t p_byte = 0;
    for (int8_t i = 0; i < 8; i++) {
        uint8_t byte_idx = (DECODE_OFFSET + 34 + i) / 8;
        uint8_t bit_idx = 7 - ((DECODE_OFFSET + 34 + i) % 8);
        bool bit = (decoded[byte_idx] >> bit_idx) & 1;
        p_byte = (p_byte << 1) | bit;
    }
    *pressure = (uint8_t)((float)(p_byte - 40) * 2.48f);

    // Extract temperature (payload bits 50-57)
    uint8_t t_byte = 0;
    for (int8_t i = 0; i < 8; i++) {
        uint8_t byte_idx = (DECODE_OFFSET + 50 + i) / 8;
        uint8_t bit_idx = 7 - ((DECODE_OFFSET + 50 + i) % 8);
        bool bit = (decoded[byte_idx] >> bit_idx) & 1;
        t_byte = (t_byte << 1) | bit;
    }
    *temp = (int8_t)(t_byte - 40);

    // Verify CRC over first 58 payload bits (decoded[1..58])
    uint8_t crc_buf[8];
    memset(crc_buf, 0, 8);
    for (uint16_t i = 0; i < 58; i++) {
        uint8_t byte_idx = (DECODE_OFFSET + i) / 8;
        uint8_t bit_idx = 7 - ((DECODE_OFFSET + i) % 8);
        bool bit = (decoded[byte_idx] >> bit_idx) & 1;
        if (bit) {
            crc_buf[(6 + i) / 8] |= (1 << (7 - ((6 + i) % 8)));
        }
    }
    uint8_t crc = calculate_crc8_pmv(crc_buf, 8);

    uint8_t rx_crc = 0;
    for (int8_t i = 0; i < 8; i++) {
        uint8_t byte_idx = (DECODE_OFFSET + 58 + i) / 8;
        uint8_t bit_idx = 7 - ((DECODE_OFFSET + 58 + i) % 8);
        bool bit = (decoded[byte_idx] >> bit_idx) & 1;
        rx_crc = (rx_crc << 1) | bit;
    }

    if (crc != rx_crc) {
        Serial.printf("[DECODE] CRC mismatch: calc=0x%02X rx=0x%02X (ID bits=0x%08lX)\n", crc, rx_crc, (unsigned long)*id);
        return false;
    }

    Serial.printf("[DECODE] DM decoded %d bits\n", decoded_len);
    Serial.printf("[DECODE] ID=%08lX P_byte=%d(%dkPa) T_byte=%d(%dC)\n",
                  (unsigned long)*id, p_byte, *pressure, t_byte, *temp);
    Serial.printf("[DECODE] CRC calc=0x%02X rx=0x%02X OK\n", crc, rx_crc);

    return true;
}

void sniffer_loop() {
    if (!snifferActive) return;

    if (millis() - snifferStartMs > SNIFFER_TIMEOUT_MS) {
        Serial.println("[SNIFFER] Timeout - stopping");
        sniffer_stop();
        return;
    }

    static uint8_t acc_buf[256];
    static uint16_t acc_len = 0;
    static bool rxRunning = false;
    static uint32_t lastDiag = 0;

    uint32_t now = millis();

    // Step 1: Start RX once, keep it running
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

    // Step 1b: Check if CC1101 left RX — restart if needed
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

    // Step 2: Read all available FIFO data
    uint8_t rx_bytes = cc1101.getRxBytes() & 0x7F;
    if (rx_bytes > 0) {
        if (rx_bytes > 64) rx_bytes = 64;
        if (acc_len + rx_bytes <= sizeof(acc_buf)) {
            cc1101.readBurstReg(CC1101_RX_FIFO, acc_buf + acc_len, rx_bytes);
            acc_len += rx_bytes;
        }
    }

    // Step 3: Try decode whenever we have enough data
    if (acc_len >= 10) {
        int8_t rssi = cc1101.getRssi();
        // Diagnostic: print RSSI and FIFO bytes every 5s even if no decode
        if (now - lastDiag >= 5000) {
            lastDiag = now;
            uint8_t marc = cc1101.getMarcState();
            uint8_t fifo = cc1101.getRxBytes() & 0x7F;
            Serial.printf("[DIAG-LOOP] RSSI=%d MARC=0x%02x FIFO=%d acc_len=%d\n", rssi, marc, fifo, acc_len);
            // Dump first 32 bytes of accumulator for analysis
            if (acc_len >= 32) {
                Serial.print("[HEX] ");
                for (uint8_t i = 0; i < 32 && i < acc_len; i++) {
                    Serial.printf("%02X ", acc_buf[i]);
                }
                Serial.println();
            }
        }
        if (rssi > -95) {
            uint32_t id = 0;
            uint8_t pressure = 0;
            int8_t temp = 0;
            if (sniffer_decode_packet(acc_buf, acc_len, &id, &pressure, &temp)) {
                Serial.printf("*** DECODE OK: ID=%08lx P=%d T=%d RSSI=%d (%d bytes)\n",
                              (unsigned long)id, pressure, temp, rssi, acc_len);
                wsSendLog("[SNIFFER] DECODE OK: ID=" + String(id, HEX) +
                          " P=" + String(pressure) + "kPa T=" + String(temp) + "C");

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

                    char id_buf[9];
                    snprintf(id_buf, sizeof(id_buf), "%08lx", (unsigned long)id);
                    Serial.printf("*** FOUND sensor #%d: ID=%s P=%dkPa T=%dC\n",
                                  sniffer_count, id_buf, pressure, temp);
                    wsSendLog("[SNIFFER] Found sensor: ID=" + String(id_buf) +
                              " P=" + String(pressure) + "kPa T=" + String(temp) + "C");
                    wsBroadcastStatus();

                    if (sniffer_count >= SNIFFER_MAX_SENSORS || sniffer_count >= TPMS_NUM_SENSORS) {
                        Serial.println("[SNIFFER] All sensors found, stopping");
                        sniffer_stop();
                        return;
                    }
                }
                acc_len = 0;
            }
        }
    }

    // Step 4: Prevent overflow
    if (acc_len >= sizeof(acc_buf) - 64) {
        uint16_t keep = 64;
        memmove(acc_buf, acc_buf + (acc_len - keep), keep);
        acc_len = keep;
    }

    // Auto-stop if no new discoveries in 60s
    if (sniffer_count > 0 && (millis() - sniffer_discovery_ms) > 60000) {
        Serial.println("[SNIFFER] No new sensors for 60s, stopping");
        sniffer_stop();
    }

}

// ============================================================================
// CC1101 Raw Packet Transmission (Fixed Length Mode)
// ============================================================================
void cc1101_send_raw(const uint8_t *data, uint8_t len) {
    cc1101.setIdleState();
    delay(1);

    cc1101.sendCommand(CC1101_SCAL);
    delay(3);

    // Fixed length TX mode
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

// ============================================================================
// MARCSTATE decoder
// ============================================================================

// ============================================================================
// Print Configuration
// ============================================================================

// ============================================================================
// EEPROM Configuration
// ============================================================================
void init_config() {
    config.magic = EEPROM_MAGIC;
    config.datarate = TPMS_DEFAULT_DATARATE;
    config.deviation = (uint16_t)(TPMS_DEFAULT_DEVIATION * 10);
    config.power = TPMS_DEFAULT_POWER;
    config.tx_enabled = 1;  // auto-TX ON by default
    config.freq = 0;        // 315 MHz default
    memset(config.reserved, 0, 3);
    config.autoTxInterval = 5;   // 5 sec default for testing
    config.autoTxPackets = 2;     // 2 packets per sensor (v6 optimized)
    config.reserved2 = 0;
    config.battMah = BATT_DEFAULT_MAH;
    ee_buf[EE_LICENSE] = 0;  // trial mode for fresh install (blank EEPROM = 0xFF → 0)

    for (uint8_t i = 0; i < TPMS_NUM_SENSORS; i++) {
        config.sensors[i].sensor_id = DEFAULT_SENSOR_IDS[i];
        config.sensors[i].pressure_kpa = 230;  // ~33 PSI
        config.sensors[i].temperature_c = 20;
        config.sensors[i].battery_ok = 0;      // always OK
        config.sensors[i].flags = 0x01;        // enabled by default
    }
}

void load_config() {
    for (int i = 0; i < EE_TOTAL; i++) ee_buf[i] = EEPROM.read(i);

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

        // Battery capacity: default 3000 if EEPROM blank (0xFFFF)
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

        // Ensure all sensors enabled
        for (uint8_t i = 0; i < TPMS_NUM_SENSORS; i++) {
            if (config.sensors[i].flags == 0) config.sensors[i].flags = 0x01;
        }
    }
}

void save_config() {
    flush_trial();  // ensure unwritten trial seconds are saved before writing all bytes
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

    // License byte: blank EEPROM = 0xFF, we want 0 unless mark_licensed() was called
    if (ee_buf[EE_LICENSE] != 0xFF) ee_buf[EE_LICENSE] = 0;

    // Trial counter: blank EEPROM = 0xFFFFFFFF, init to 0
    if (ee_read32(EE_TRIAL_SEC) == 0xFFFFFFFF) {
        ee_write32(EE_TRIAL_SEC, 0);
    }

    for (int i = 0; i < EE_TOTAL; i++) EEPROM.write(i, ee_buf[i]);
    EEPROM.commit();
}

// ============================================================================
// Send burst to all enabled sensors
// ============================================================================
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
// Enter deep sleep
// ============================================================================
void enterDeepSleep() {
    cc1101_power_off();
    Serial.println("[SLEEP] Entering deep sleep...");
    // Save trial seconds before sleep
    flush_trial();
    // Turn off WiFi
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(10);
    // Deep sleep for autoTxInterval seconds
#if defined(ESP8266)
    ESP.deepSleep((uint32_t)autoTxInterval * 1000000UL);
#else
    esp_sleep_enable_timer_wakeup((uint64_t)autoTxInterval * 1000000ULL);
    esp_deep_sleep_start();
#endif
}

// ============================================================================
// Setup
// ============================================================================
void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println("[SETUP] TPMS Emulator v7 starting...");

    // CC1101 power control pin
    pinMode(PIN_CC1101_POWER, OUTPUT);
    Serial.println("[SETUP] CC1101 power pin configured");

    EEPROM.begin(512);
    load_config();
    Serial.println("[SETUP] Config loaded");

    // Force auto-TX ON
    autoTxEnabled = true;
    config.tx_enabled = 1;
    autoTxInterval = 5;

    // Power on and init CC1101
    cc1101_power_on();
    Serial.println("[SETUP] CC1101 powered on");
    init_cc1101();
    Serial.println("[SETUP] CC1101 initialized");

    // Check if waking from deep sleep
#if defined(ESP8266)
    bool fromDeepSleep = (ESP.getResetReason() == "Deep Sleep Wake");
#else
    bool fromDeepSleep = (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER);
#endif
    Serial.printf("[SETUP] Deep sleep wakeup: %s\n", fromDeepSleep ? "YES" : "NO");

    if (fromDeepSleep) {
        // Quick wake: CC1101 already powered on and initialized above,
        // just send burst and go back to sleep immediately (NO WiFi on wake)
        // Add deep-sleep time to trial counter (direct write — trial_unwritten resets on boot)
        if (!is_licensed()) {
            uint32_t total = get_trial_seconds() + autoTxInterval;
            if (total > 86400) total = 86400;
            ee_write32(EE_TRIAL_SEC, total);
            for (int i = 0; i < 4; i++) EEPROM.write(EE_TRIAL_SEC + i, ee_buf[EE_TRIAL_SEC + i]);
            EEPROM.commit();
            // If trial just expired, don't send — cut power immediately
            if (is_trial_expired()) {
                Serial.println("[TRIAL] EXPIRED during wake — skipping TX, powering OFF");
                cc1101_power_off();
#if defined(ESP8266)
                ESP.deepSleep((uint32_t)autoTxInterval * 1000000UL);
#else
                esp_sleep_enable_timer_wakeup((uint64_t)autoTxInterval * 1000000ULL);
                esp_deep_sleep_start();
#endif
                return;
            }
        }
        // Check battery — if critical, don't send, just sleep
        if (isBatteryCritical()) {
            Serial.println("[BATT] Critical during wake — skipping TX, deep sleep");
            cc1101_power_off();
#if defined(ESP8266)
            ESP.deepSleep((uint32_t)autoTxInterval * 1000000UL);
#else
            esp_sleep_enable_timer_wakeup((uint64_t)autoTxInterval * 1000000ULL);
            esp_deep_sleep_start();
#endif
            return;
        }
        send_burst_all();
        cc1101_power_off();
        // Both boards: deep sleep again WITHOUT WiFi
        Serial.printf("[SLEEP] Wake cycle done, deep sleep %ds...\n", autoTxInterval);
#if defined(ESP8266)
        ESP.deepSleep((uint32_t)autoTxInterval * 1000000UL);
#else
        esp_sleep_enable_timer_wakeup((uint64_t)autoTxInterval * 1000000ULL);
        esp_deep_sleep_start();
#endif
        return;
    }

    // Cold boot: start WiFi for configuration
    Serial.println("[SETUP] Starting WiFi...");
    setupWiFi();
    Serial.println("[SETUP] WiFi started");
    setupWebServer();
    Serial.println("[SETUP] Web server setup");
    setupWebSocket();
    Serial.println("[SETUP] WebSocket setup");

    wifiActive = true;
    wifiTimeoutMs = millis() + WIFI_CONFIG_TIMEOUT_MS;
    Serial.println("[SETUP] Setup complete, starting loop");

    // Send initial burst
    send_burst_all();
    autoTxLastMs = millis();

#if !defined(ESP8266)
    // Sniffer starts manually via web UI or serial 'sniff_start'
    // (Auto-start was removed — WiFi gets killed, no time to access web UI)
#endif
}

// ============================================================================
// Loop
// ============================================================================
void loop() {
    if (!wifiActive) {
        // Still run sniffer even without WiFi
        sniffer_loop();
        delay(1);
        return;
    }

    dnsServer.processNextRequest();
    server.handleClient();
    wsServer.loop();

    // Debug: print connected clients count every 5 seconds
    static uint32_t lastDebugPrint = 0;
    if (millis() - lastDebugPrint > 5000) {
        lastDebugPrint = millis();
        Serial.printf("[LOOP] WiFi clients: %d, WS clients: %d\n",
                      WiFi.softAPgetStationNum(), wsClients);
    }

    // Track trial time (add 30s every 30s while WiFi is on)
    static uint32_t lastTrialUpdate = 0;
    static bool trialExpiredPowerOff = false;
    if (!is_licensed()) {
        uint32_t now = millis();
        if (now - lastTrialUpdate >= 30000) {
            uint32_t elapsed = (now - lastTrialUpdate) / 1000;
            if (elapsed > 0) add_trial_seconds(elapsed);
            lastTrialUpdate = now;
        }
        // When trial expires: cut CC1101 power immediately
        if (is_trial_expired() && !trialExpiredPowerOff) {
            trialExpiredPowerOff = true;
            if (snifferActive) sniffer_stop();
            cc1101_power_off();
            Serial.println("[TRIAL] EXPIRED — CC1101 powered OFF");
            wsSendLog("[TRIAL] License expired — CC1101 disabled. Contact: wargaelanor@yandex.ru");
        }
    }

    // Sniffer mode
    sniffer_loop();

#if !defined(ESP8266)
    // DIAG-TX REMOVED: was flooding channel every 30s, blocking RX
#endif // !ESP8266

    // Serial commands
    if (Serial.available()) {
        char cmd = Serial.read();
        if (cmd == 'n') {
            // Start sniffer via serial
#if !defined(ESP8266)
            if (!snifferActive) {
                Serial.println("[CMD] Starting sniffer...");
                sniffer_start();
            } else {
                Serial.println("[CMD] Sniffer already active, stopping...");
                sniffer_stop();
            }
#endif
        } else if (cmd == 's') {
            // Wide frequency sweep to detect crystal mismatch
            Serial.println("[CMD] Wide frequency sweep...");
            bool wasSniffing = snifferActive;
            if (wasSniffing) sniffer_stop();
            cc1101.setIdleState();
            delay(1);
            int8_t prevRssi = -128;
            for (float f = 280.0f; f < 370.0f; f += 0.5f) {
                cc1101.setFreq(f);
                cc1101.sendCommand(CC1101_SCAL);
                delay(2);
                cc1101.flushRxFifo();
                cc1101.setRxState();
                delay(20);
                int8_t rssi = cc1101.getRssi();
                if (abs(rssi - prevRssi) >= 3) {
                    Serial.printf("[SWEEP] %.1f MHz -> RSSI=%d dBm\n", f, rssi);
                    prevRssi = rssi;
                }
            }
            // Restore
            cc1101.setIdleState();
            float freq = (config.freq == 1) ? TPMS_FREQ_433 : TPMS_FREQ_315;
            cc1101.setFreqConfig(freq);
            cc1101.setFreq(freq);
            cc1101.setRxConfig();
            cc1101.setModulation(0);  // 2-FSK
            cc1101.setSyncMode(0);
            set_cc1101_data_rate(config.datarate);
            set_cc1101_deviation(38.0f);
            cc1101.sendCommand(CC1101_SCAL);
            delay(3);
            cc1101.flushRxFifo();
            cc1101.setRxState();
            Serial.println("[CMD] Sweep done — restart sniffer from web UI if needed");
        } else if (cmd == 't') {
            // Self-echo with a REAL PMV-107J sensor packet at low power
            Serial.println("[CMD] Self-echo (PMV-107J real packet, low power)...");
            cc1101.setIdleState();
            delay(2);
            cc1101.setPA(1);  // minimum TX power to reduce RX saturation
            send_pmv107j_sensor(0);  // transmit sensor 0 as TPMS packet
            delay(50);  // wait for TX to complete
            cc1101.setIdleState();
            delay(2);
            cc1101.flushTxFifo();
            delay(5);  // extra gap for carrier decay
            float freq_save = (config.freq == 1) ? TPMS_FREQ_433 : TPMS_FREQ_315;
            cc1101.setFreqConfig(freq_save);
            cc1101.setFreq(freq_save);
            cc1101.setRxConfig();
            cc1101.setModulation(0);
            cc1101.setSyncMode(0);
            set_cc1101_data_rate(config.datarate);
            set_cc1101_deviation(38.0f);
            cc1101.sendCommand(CC1101_SCAL);
            delay(5);
            cc1101.flushRxFifo();
            cc1101.setRxState();
            delay(500);
            uint8_t rx = cc1101.getRxBytes() & 0x7F;
            int8_t r = cc1101.getRssi();
            Serial.printf("[CMD] Self-echo: FIFO=%d bytes, RSSI=%d dBm\n", rx, r);
            if (rx > 0) {
                if (rx > 64) rx = 64;
                uint8_t buf[64];
                cc1101.readBurstReg(CC1101_RX_FIFO, buf, rx);
                for (int i = 0; i < rx; i++) Serial.printf("%02X ", buf[i]);
                Serial.println();
                // Attempt decode
                uint32_t id = 0; uint8_t pressure = 0; int8_t temp = 0;
                if (sniffer_decode_packet(buf, rx, &id, &pressure, &temp)) {
                    Serial.printf("[CMD] DECODE OK: ID=%08lx P=%d T=%d\n", id, pressure, temp);
                } else {
                    Serial.println("[CMD] DECODE FAIL");
                }
            }
            cc1101.setPA(config.power);  // restore configured TX power
            Serial.println("[CMD] Done");
        } else if (cmd == 'r') {
            // Reset trial timer (for development)
            ee_write32(EE_TRIAL_SEC, 0);
            for (int i = 0; i < 4; i++) EEPROM.write(EE_TRIAL_SEC + i, ee_buf[EE_TRIAL_SEC + i]);
            EEPROM.commit();
            trial_unwritten = 0;
            Serial.println("[CMD] Trial timer reset to 0");
        }
    }

#if !defined(ESP8266)
    // RSSI monitor — ESP32 only, skip during sniffer (sniffer has its own diag)
    if (!snifferActive) {
        static uint32_t lastRssiPrint = 0;
        static int8_t lastRssi = -128;
        if (millis() - lastRssiPrint > 500) {
            lastRssiPrint = millis();
            int8_t now = cc1101.getRssi();
            if (abs(now - lastRssi) >= 3) {
                Serial.printf("[RSSI] %d dBm\n", now);
                lastRssi = now;
            }
        }
    }
#endif // !ESP8266

    // Check WiFi timeout (if no WS clients and not sniffing)
    if (millis() > wifiTimeoutMs && wsClients == 0 && !snifferActive) {
        enterDeepSleep();
    }

    // Auto-TX while WiFi is active (skip if sniffing)
    if (!snifferActive) {
        runAutoTx();
    }

}

// ============================================================================
// WiFi AP Setup
// ============================================================================
void setupWiFi() {
    Serial.println("[WIFI] Setting up AP...");
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    bool apResult = WiFi.softAP(AP_SSID, AP_PASS, 1, 0, 4);  // channel 1, hidden=0, max_conn=4
    Serial.printf("[WIFI] softAP result: %s\n", apResult ? "OK" : "FAIL");
    delay(100);
    Serial.printf("[WIFI] AP IP: %s\n", WiFi.softAPIP().toString().c_str());
    Serial.printf("[WIFI] AP MAC: %s\n", WiFi.softAPmacAddress().c_str());
    Serial.printf("[WIFI] Connected stations: %d\n", WiFi.softAPgetStationNum());
    dnsServer.start(DNS_PORT, "*", apIP);
    Serial.println("[WIFI] DNS server started");
}

// ============================================================================
// Embedded Web UI (PROGMEM — no LittleFS needed)
// ============================================================================
// ============================================================================
const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="Cache-Control" content="no-cache,no-store,must-revalidate">
<meta http-equiv="Pragma" content="no-cache">
<meta http-equiv="Expires" content="0">
<title>TPMS Emulator v5</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
:root{--primary:#7c4dff;--success:#69f0ae;--warning:#ffd740;--error:#ff5252;
  --card-bg:rgba(255,255,255,0.06);--card-border:rgba(255,255,255,0.1);--radius:12px;
  --wheel-on:#69f0ae;--wheel-off:#555;--body-fill:rgba(124,77,255,0.35);--body-stroke:#7c4dff}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;
  background:linear-gradient(135deg,#1a0533 0%,#0d1b3e 50%,#0a1628 100%);
  color:#e0e0e0;min-height:100vh;padding:12px;background-attachment:fixed}
.container{max-width:600px;margin:0 auto}
.header{text-align:center;padding:10px 0 14px}
.header h1{font-size:20px;font-weight:700}
.header .sub{font-size:12px;color:#9e9e9e;margin-top:2px}

/* Car layout: 2x2 grid matching real wheel positions */
.car{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:12px}
.sensor-card{background:var(--card-bg);border:1px solid var(--card-border);
  border-radius:var(--radius);padding:14px;backdrop-filter:blur(10px);
  border-left:3px solid var(--primary);transition:border-color .3s}
.sensor-card.active{border-left-color:var(--success)}
.sensor-card .pos{font-size:11px;color:#9e9e9e;text-transform:uppercase;letter-spacing:.5px;
  font-weight:600;margin-bottom:8px;display:flex;align-items:center;gap:6px}

/* Car icon per card */
.wheel-icon{width:34px;height:38px;display:flex;align-items:center;
  justify-content:center;flex-shrink:0;transition:all .3s}
.wheel-icon svg{width:34px;height:38px}

/* Fields */
.field{margin-bottom:6px}
.field label{display:block;font-size:10px;color:#9e9e9e;margin-bottom:2px;text-transform:uppercase;letter-spacing:.3px}
.field input{width:100%;padding:6px 8px;background:rgba(0,0,0,0.3);border:1px solid rgba(255,255,255,0.1);
  border-radius:6px;color:#fff;font-size:13px;font-family:inherit;outline:none;transition:border .2s}
.field input:focus{border-color:var(--primary)}
.field-row{display:flex;gap:8px}
.field-row .field{flex:1}
.field-id input{font-family:monospace;font-size:12px;letter-spacing:1px;text-transform:uppercase}

/* Toggle */
.toggle-wrap{display:flex;align-items:center;gap:8px;margin:6px 0 8px}
.toggle{position:relative;width:40px;height:22px;cursor:pointer}
.toggle input{opacity:0;width:0;height:0}
.toggle .slider{position:absolute;inset:0;background:rgba(255,255,255,0.1);border-radius:11px;transition:.2s}
.toggle .slider::before{content:'';position:absolute;width:16px;height:16px;left:3px;top:3px;background:#666;border-radius:50%;transition:.2s}
.toggle input:checked+.slider{background:rgba(105,240,174,0.3)}
.toggle input:checked+.slider::before{transform:translateX(18px);background:var(--success)}
.toggle-label{font-size:12px;user-select:none}

/* Buttons */
.btn-row{display:flex;gap:6px;margin-top:8px}
.btn{padding:6px 12px;border:none;border-radius:6px;font-size:11px;font-weight:600;
  cursor:pointer;transition:all .15s;font-family:inherit;flex:1}
.btn:active{transform:scale(0.97)}
.btn:disabled{opacity:0.5;cursor:not-allowed}
.btn-primary{background:var(--primary);color:#fff}
.btn-primary:hover{background:#651fff}
.btn-success{background:#00c853;color:#fff}
.btn-success:hover{background:#00e676}
.btn-warning{background:#ff9100;color:#fff}
.btn-warning:hover{background:#ffab40}
.btn-danger{background:var(--error);color:#fff}
.btn-danger:hover{background:#ff1744}
.btn-outline{background:transparent;border:1px solid rgba(255,255,255,0.2);color:#ccc}
.btn-outline:hover{background:rgba(255,255,255,0.05)}

/* Auto-TX card */
.atx-card{background:var(--card-bg);border:1px solid var(--card-border);
  border-radius:var(--radius);padding:14px;margin-bottom:12px;backdrop-filter:blur(10px)}
.atx-card h2{font-size:12px;color:#9e9e9e;font-weight:600;text-transform:uppercase;
  letter-spacing:.5px;margin-bottom:10px;display:flex;align-items:center;gap:6px}
.atx-card h2::before{content:'';width:3px;height:10px;background:var(--warning);border-radius:2px}
.atx-row{display:flex;gap:10px;margin-bottom:10px}
.atx-row .field{flex:1}
.save-ok{color:var(--success);font-size:11px;min-height:16px;margin-top:4px}

/* Log */
.log-box{background:rgba(0,0,0,0.3);border:1px solid rgba(255,255,255,0.08);
  border-radius:8px;padding:8px;height:140px;overflow-y:auto;font-size:10px;
  font-family:monospace;margin-bottom:12px;color:#9e9e9e;line-height:1.5}
.log-box .log-ok{color:var(--success)}
.log-box .log-err{color:var(--error)}
.loading{color:var(--warning);font-size:11px;text-align:center;padding:10px}
.tx-flash{animation:flash .4s}
@keyframes flash{0%,100%{background:var(--primary)}50%{background:#00e676}}

/* Battery indicator */
.batt{display:flex;align-items:center;gap:6px;font-size:11px;font-family:monospace;color:#69f0ae}
.batt-icon{width:22px;height:11px;border:1.5px solid #69f0ae;border-radius:2px;position:relative;flex-shrink:0}
.batt-icon::after{content:'';position:absolute;right:-5px;top:2.5px;width:3px;height:5px;background:#69f0ae;border-radius:0 1px 1px 0}
.batt-fill{height:100%;background:#69f0ae;transition:width .3s;border-radius:1px}
.batt-low{color:#ffd740}.batt-low .batt-icon{border-color:#ffd740}.batt-low .batt-icon::after{background:#ffd740}.batt-low .batt-fill{background:#ffd740}
.batt-crit{color:#ff5252}.batt-crit .batt-icon{border-color:#ff5252}.batt-crit .batt-icon::after{background:#ff5252}.batt-crit .batt-fill{background:#ff5252}


</style>
</head>
<body>
<div class="container">
<div class="header">
  <h1>TPMS Emulator</h1>
  <div class="sub">PMV-107J &middot; <span id="freqDisplay">315</span> MHz</div>
  <div class="batt" id="battWrap">
    <div class="batt-icon"><div class="batt-fill" id="battFill" style="width:85%"></div></div>
    <span id="battText">--- mV</span>
  </div>
</div>

<!-- 4 sensor cards: top-left, top-right, bottom-left, bottom-right = car layout -->
<div class="car">

  <!-- S0: PL -- Front Left -->
  <div class="sensor-card" id="card0">
    <div class="pos">
      <div class="wheel-icon" id="wicon0"></div>
      <span>&#1051;&#1077;&#1074;&#1086;&#1077; &#1055;&#1077;&#1088;&#1077;&#1076;&#1085;&#1077;&#1077;</span>
    </div>
    <div class="field field-id"><label>ID &#1076;&#1072;&#1090;&#1095;&#1080;&#1082;&#1072;</label><input type="text" id="id0" maxlength="8" placeholder="00000000"></div>
    <div class="field-row">
      <div class="field"><label>&#1044;&#1072;&#1074;&#1083;., &#1082;&#1055;&#1072;</label><input type="number" id="p0" placeholder="230" min="0" max="400"></div>
      <div class="field"><label>&#1058;&#1077;&#1084;&#1087;., &deg;C</label><input type="number" id="t0" placeholder="20" min="-40" max="85"></div>
    </div>
    <div class="toggle-wrap">
      <label class="toggle"><input type="checkbox" id="e0" checked><span class="slider"></span></label>
      <span class="toggle-label" id="el0">&#1042;&#1082;&#1083;</span>
    </div>
    <div class="btn-row">
      <button class="btn btn-primary" onclick="saveSensor(0)">&#1057;&#1086;&#1093;&#1088;&#1072;&#1085;&#1080;&#1090;&#1100;</button>
      <button class="btn btn-success" id="txbtn0" onclick="txOne(0)">&#1054;&#1090;&#1087;&#1088;&#1072;&#1074;&#1080;&#1090;&#1100;</button>
    </div>
  </div>

  <!-- S1: PP -- Front Right -->
  <div class="sensor-card" id="card1">
    <div class="pos">
      <div class="wheel-icon" id="wicon1"></div>
      <span>&#1055;&#1088;&#1072;&#1074;&#1086;&#1077; &#1055;&#1077;&#1088;&#1077;&#1076;&#1085;&#1077;&#1077;</span>
    </div>
    <div class="field field-id"><label>ID &#1076;&#1072;&#1090;&#1095;&#1080;&#1082;&#1072;</label><input type="text" id="id1" maxlength="8" placeholder="00000000"></div>
    <div class="field-row">
      <div class="field"><label>&#1044;&#1072;&#1074;&#1083;., &#1082;&#1055;&#1072;</label><input type="number" id="p1" placeholder="230" min="0" max="400"></div>
      <div class="field"><label>&#1058;&#1077;&#1084;&#1087;., &deg;C</label><input type="number" id="t1" placeholder="20" min="-40" max="85"></div>
    </div>
    <div class="toggle-wrap">
      <label class="toggle"><input type="checkbox" id="e1" checked><span class="slider"></span></label>
      <span class="toggle-label" id="el1">&#1042;&#1082;&#1083;</span>
    </div>
    <div class="btn-row">
      <button class="btn btn-primary" onclick="saveSensor(1)">&#1057;&#1086;&#1093;&#1088;&#1072;&#1085;&#1080;&#1090;&#1100;</button>
      <button class="btn btn-success" id="txbtn1" onclick="txOne(1)">&#1054;&#1090;&#1087;&#1088;&#1072;&#1074;&#1080;&#1090;&#1100;</button>
    </div>
  </div>

  <!-- S2: ZL -- Rear Left -->
  <div class="sensor-card" id="card2">
    <div class="pos">
      <div class="wheel-icon" id="wicon2"></div>
      <span>&#1051;&#1077;&#1074;&#1086;&#1077; &#1047;&#1072;&#1076;&#1085;&#1077;&#1077;</span>
    </div>
    <div class="field field-id"><label>ID &#1076;&#1072;&#1090;&#1095;&#1080;&#1082;&#1072;</label><input type="text" id="id2" maxlength="8" placeholder="00000000"></div>
    <div class="field-row">
      <div class="field"><label>&#1044;&#1072;&#1074;&#1083;., &#1082;&#1055;&#1072;</label><input type="number" id="p2" placeholder="230" min="0" max="400"></div>
      <div class="field"><label>&#1058;&#1077;&#1084;&#1087;., &deg;C</label><input type="number" id="t2" placeholder="20" min="-40" max="85"></div>
    </div>
    <div class="toggle-wrap">
      <label class="toggle"><input type="checkbox" id="e2" checked><span class="slider"></span></label>
      <span class="toggle-label" id="el2">&#1042;&#1082;&#1083;</span>
    </div>
    <div class="btn-row">
      <button class="btn btn-primary" onclick="saveSensor(2)">&#1057;&#1086;&#1093;&#1088;&#1072;&#1085;&#1080;&#1090;&#1100;</button>
      <button class="btn btn-success" id="txbtn2" onclick="txOne(2)">&#1054;&#1090;&#1087;&#1088;&#1072;&#1074;&#1080;&#1090;&#1100;</button>
    </div>
  </div>

  <!-- S3: ZP -- Rear Right -->
  <div class="sensor-card" id="card3">
    <div class="pos">
      <div class="wheel-icon" id="wicon3"></div>
      <span>&#1055;&#1088;&#1072;&#1074;&#1086;&#1077; &#1047;&#1072;&#1076;&#1085;&#1077;&#1077;</span>
    </div>
    <div class="field field-id"><label>ID &#1076;&#1072;&#1090;&#1095;&#1080;&#1082;&#1072;</label><input type="text" id="id3" maxlength="8" placeholder="00000000"></div>
    <div class="field-row">
      <div class="field"><label>&#1044;&#1072;&#1074;&#1083;., &#1082;&#1055;&#1072;</label><input type="number" id="p3" placeholder="230" min="0" max="400"></div>
      <div class="field"><label>&#1058;&#1077;&#1084;&#1087;., &deg;C</label><input type="number" id="t3" placeholder="20" min="-40" max="85"></div>
    </div>
    <div class="toggle-wrap">
      <label class="toggle"><input type="checkbox" id="e3" checked><span class="slider"></span></label>
      <span class="toggle-label" id="el3">&#1042;&#1082;&#1083;</span>
    </div>
    <div class="btn-row">
      <button class="btn btn-primary" onclick="saveSensor(3)">&#1057;&#1086;&#1093;&#1088;&#1072;&#1085;&#1080;&#1090;&#1100;</button>
      <button class="btn btn-success" id="txbtn3" onclick="txOne(3)">&#1054;&#1090;&#1087;&#1088;&#1072;&#1074;&#1080;&#1090;&#1100;</button>
    </div>
  </div>

</div>

<!-- Frequency selector -->
<div class="atx-card">
  <h2>&#1063;&#1072;&#1089;&#1090;&#1086;&#1090;&#1072; &#1080; &#1072;&#1074;&#1090;&#1086;-&#1086;&#1090;&#1087;&#1088;&#1072;&#1074;&#1082;&#1072;</h2>
  <div class="atx-row">
    <div class="field"><label>&#1063;&#1072;&#1089;&#1090;&#1086;&#1090;&#1072;, &#1052;&#1043;&#1094;</label>
      <select id="freqSelect" style="width:100%;padding:6px 8px;background:rgba(0,0,0,0.3);border:1px solid rgba(255,255,255,0.1);border-radius:6px;color:#fff;font-size:13px;font-family:inherit;outline:none">
        <option value="315">315</option>
        <option value="433">433</option>
      </select>
    </div>
    <div class="field"><label>&nbsp;</label><button class="btn btn-outline" onclick="applyFreq()" style="width:100%">&#1055;&#1088;&#1080;&#1084;&#1077;&#1085;&#1080;&#1090;&#1100;</button></div>
  </div>
  <div class="atx-row">
    <div class="field"><label>&#1048;&#1085;&#1090;&#1077;&#1088;&#1074;&#1072;&#1083;, &#1089;&#1077;&#1082;</label><input type="number" id="atInterval" placeholder="300" min="5" max="900"></div>
    <div class="field"><label>&#1055;&#1072;&#1082;&#1077;&#1090;&#1086;&#1074; &#1085;&#1072; &#1076;&#1072;&#1090;&#1095;&#1080;&#1082;</label><input type="number" id="atPackets" placeholder="2" min="1" max="20"></div>
  </div>
  <div class="btn-row">
    <button class="btn btn-warning" onclick="cmdSend('burst')">Burst</button>
    <button class="btn btn-success" onclick="cmdSend('tx')">&#1057;&#1090;&#1072;&#1088;&#1090;</button>
    <button class="btn btn-danger" onclick="cmdSend('stop')">&#1057;&#1090;&#1086;&#1087;</button>
    <button class="btn btn-outline" onclick="applyAutoTx()">&#1055;&#1088;&#1080;&#1084;&#1077;&#1085;&#1080;&#1090;&#1100;</button>
  </div>
  <div class="save-ok" id="atxMsg"></div>
  <div id="batteryEst" style="margin-top:6px;font-size:10px;color:#69f0ae;font-family:monospace"></div>
</div>

<!-- Sniffer Mode -->
<div class="atx-card" id="snifferCard">
  <h2>&#1057;&#1085;&#1080;&#1092;&#1092;&#1077;&#1088; &#1076;&#1072;&#1090;&#1095;&#1080;&#1082;&#1086;&#1074;</h2>
  <div class="btn-row">
    <button class="btn btn-success" id="sniffStartBtn" onclick="cmdSend('sniff_start')">&#1057;&#1090;&#1072;&#1088;&#1090;</button>
    <button class="btn btn-danger" id="sniffStopBtn" onclick="cmdSend('sniff_stop')" disabled>&#1057;&#1090;&#1086;&#1087;</button>
    <button class="btn btn-primary" id="sniffApplyBtn" onclick="cmdSend('sniff_apply')" disabled>&#1055;&#1088;&#1080;&#1084;&#1077;&#1085;&#1080;&#1090;&#1100;</button>
  </div>
  <div class="save-ok" id="sniffMsg"></div>
  <div id="sniffResults" style="margin-top:8px;font-size:11px;font-family:monospace;color:#9e9e9e"></div>
</div>

<!-- License -->
<div class="atx-card">
  <h2>&#1051;&#1080;&#1094;&#1077;&#1085;&#1079;&#1080;&#1103;</h2>
  <div style="font-size:11px;color:#9e9e9e;margin-bottom:6px;font-family:monospace" id="licenseDeviceId">ID: ---</div>
  <div class="field" style="margin-bottom:6px">
    <label>&#1050;&#1083;&#1102;&#1095; (8 &#1089;&#1080;&#1084;&#1074;&#1086;&#1083;&#1086;&#1074;, HEX)</label>
    <input type="text" id="licenseKey" maxlength="8" placeholder="A1B2C3D4" style="text-transform:uppercase;font-family:monospace;letter-spacing:2px">
  </div>
  <div class="btn-row">
    <button class="btn btn-success" onclick="applyLicense()">&#1040;&#1082;&#1090;&#1080;&#1074;&#1080;&#1088;&#1086;&#1074;&#1072;&#1090;&#1100;</button>
  </div>
  <div class="save-ok" id="licenseMsg"></div>
  <div style="margin-top:6px;font-size:10px;color:#9e9e9e">&#1047;&#1072; &#1082;&#1083;&#1102;&#1095;&#1086;&#1084;: <a href="mailto:wargaelanor@yandex.ru" style="color:#7c4dff">wargaelanor@yandex.ru</a></div>
</div>

<!-- OTA Update -->
<div class="atx-card">
  <h2>&#1054;&#1073;&#1085;&#1086;&#1074;&#1083;&#1077;&#1085;&#1080;&#1077; &#1087;&#1088;&#1086;&#1096;&#1080;&#1074;&#1082;&#1080; (OTA)</h2>
  <div style="font-size:11px;color:#9e9e9e;margin-bottom:8px">&#1042;&#1099;&#1073;&#1077;&#1088;&#1080;&#1090;&#1077; .bin &#1092;&#1072;&#1081;&#1083; &#1087;&#1088;&#1086;&#1096;&#1080;&#1074;&#1082;&#1080; &#1080; &#1085;&#1072;&#1078;&#1084;&#1080;&#1090;&#1077; "Upload"</div>
  <div class="field" style="margin-bottom:8px">
    <input type="file" id="otaFile" accept=".bin" style="font-size:12px;color:#9e9e9e">
  </div>
  <div class="btn-row">
    <button class="btn btn-warning" id="otaBtn" onclick="otaUpload()">&#1047;&#1072;&#1075;&#1088;&#1091;&#1079;&#1080;&#1090;&#1100;</button>
  </div>
  <div id="otaProgress" style="margin-top:8px;display:none">
    <div style="background:rgba(255,255,255,0.1);border-radius:4px;height:6px;overflow:hidden">
      <div id="otaBar" style="background:var(--warning);height:100%;width:0%;transition:width .3s"></div>
    </div>
    <div id="otaStatus" style="font-size:10px;color:#9e9e9e;margin-top:4px;font-family:monospace"></div>
  </div>
  <div class="save-ok" id="otaMsg"></div>
</div>

<!-- Log -->
<div class="log-box" id="logBox"><div class="loading">&#1047;&#1072;&#1075;&#1088;&#1091;&#1079;&#1082;&#1072;...</div></div>

</div>

<script>
var $=function(id){return document.getElementById(id)};
var ws,reconnectTimer,statusLoaded=false;

/* ======== SVG Car Icon with highlighted wheel ======== */
function carSVG(corner,on){
  var w=on?'#69f0ae':'#555';
  var wo=on?'#69f0ae':'#444';
  var bo='rgba(124,77,255,0.35)';
  var bs='#7c4dff';
  /* wheel positions: FL(0),FR(1),RL(2),RR(3) — top-down car view */
  var wheels=[
    [16,18],[52,18],  /* front */
    [16,58],[52,58]   /* rear */
  ];
  var wx=wheels[corner][0],wy=wheels[corner][1];
  var s='<svg viewBox="0 0 68 76" style="width:100%;height:100%">';
  /* car body */
  s+='<path d="M18,12 L50,12 Q58,12 58,20 L58,56 Q58,64 50,64 L18,64 Q10,64 10,56 L10,20 Q10,12 18,12 Z" fill="'+bo+'" stroke="'+bs+'" stroke-width="1.5"/>';
  /* windshield */
  s+='<rect x="20" y="22" width="28" height="10" rx="3" fill="rgba(255,255,255,0.08)" stroke="rgba(255,255,255,0.15)" stroke-width="0.8"/>';
  /* rear window */
  s+='<rect x="22" y="48" width="24" height="8" rx="2" fill="rgba(255,255,255,0.06)" stroke="rgba(255,255,255,0.1)" stroke-width="0.8"/>';
  /* all 4 wheels (dim) */
  for(var i=0;i<4;i++){
    var x=wheels[i][0],y=wheels[i][1];
    s+='<rect x="'+(x-7)+'" y="'+(y-5)+'" width="14" height="10" rx="2" fill="#333" stroke="#555" stroke-width="0.8"/>';
  }
  /* highlighted wheel (on top) */
  if(on){
    s+='<rect x="'+(wx-8)+'" y="'+(wy-6)+'" width="16" height="12" rx="2.5" fill="'+w+'" fill-opacity="0.35" stroke="'+w+'" stroke-width="1.5"/>';
    s+='<circle cx="'+wx+'" cy="'+wy+'" r="3" fill="'+w+'" fill-opacity="0.6"/>';
  }else{
    s+='<rect x="'+(wx-7)+'" y="'+(wy-5)+'" width="14" height="10" rx="2" fill="#333" stroke="'+wo+'" stroke-width="1"/>';
  }
  s+='</svg>';
  return s;
}
function updateWheel(i,on){
  var el=$('wicon'+i);
  if(!el)return;
  el.innerHTML=carSVG(i,on);
  var card=$('card'+i);
  if(card){on?card.classList.add('active'):card.classList.remove('active')}
}

/* ======== WebSocket ======== */
function initWs(){
  if(ws&&ws.readyState<2)return;
  ws=new WebSocket((location.protocol==='https:'?'wss:':'ws:')+'//'+location.hostname+':81/ws');
  ws.onopen=function(){loadStatus();wsSend({cmd:'status'})};
  ws.onclose=function(){reconnectTimer=setTimeout(initWs,3000)};
  ws.onerror=function(){};
  ws.onmessage=function(e){try{handleMsg(JSON.parse(e.data))}catch(x){}};
}
function wsSend(o){if(ws&&ws.readyState===1)ws.send(JSON.stringify(o))}
function handleMsg(m){
  if(m.t==='log')logMsg(m.m);
  else if(m.t==='status')applyStatus(m.data);
  else if(m.t==='pkt'){
    logMsg('pkt #'+m.s+' ID:'+m.id+' P:'+m.p+'kPa T:'+m.t+'C cnt:'+m.c,'ok');
    loadStatus(1);
  }
}

/* ======== Status to form fields ======== */
function applyStatus(d){
  if(!d)return;
  statusLoaded=true;
  if(d.sensors){
    d.sensors.forEach(function(s,i){
      if(s.id!=null&&s.id!=='')$('id'+i).value=s.id;
      if(s.pressure!=null)$('p'+i).value=s.pressure;
      if(s.temp!=null)$('t'+i).value=s.temp;
      if(s.enabled!=null){
        $('e'+i).checked=!!s.enabled;
        $('el'+i).textContent=s.enabled?'\u0412\u043a\u043b':'\u0412\u044b\u043a\u043b';
        updateWheel(i,s.enabled);
      }
    });
  }
  if(d.freq!=null){
    $('freqSelect').value=d.freq;
    $('freqDisplay').textContent=d.freq;
  }
  if(d.autoTx){
    if(d.autoTx.interval!=null)$('atInterval').value=d.autoTx.interval;
    if(d.autoTx.packets!=null)$('atPackets').value=d.autoTx.packets;
    calcBattery();
  }
  if(d.license){
    // ESP8266 has no license system — show "always activated"
    if(d.platform==='esp8266'){
      $('licenseDeviceId').textContent='ID: '+d.license.deviceId;
      $('licenseMsg').textContent='\u2713 \u0410\u043a\u0442\u0438\u0432\u0438\u0440\u043e\u0432\u0430\u043d\u043e \u043f\u043e\u0441\u0442\u043e\u044f\u043d\u043d\u043e (ESP8266)';
      $('licenseMsg').style.color='#69f0ae';
      $('licenseKey').disabled=true;
      $('licenseKey').value='\u2014\u2014\u2014\u2014';
      return;
    }
    $('licenseDeviceId').textContent='ID: '+d.license.deviceId;
    var licMsg=$('licenseMsg');
    if(d.license.licensed){
      licMsg.textContent='\u2713 \u041b\u0438\u0446\u0435\u043d\u0437\u0438\u044f \u0430\u043a\u0442\u0438\u0432\u0438\u0440\u043e\u0432\u0430\u043d\u0430';
      $('licenseKey').disabled=true;
    }else{
      var remain=d.license.trialMax-d.license.trialSec;
      if(remain>0){
        var h=Math.floor(remain/3600);
        var m=Math.floor((remain%3600)/60);
        licMsg.textContent='\u23f1 \u041f\u0440\u043e\u0431\u043d\u044b\u0439 \u043f\u0435\u0440\u0438\u043e\u0434: '+h+'\u0447 '+m+'\u043c\u0438\u043d (\u043e\u0441\u0442\u0430\u043b\u043e\u0441\u044c)';
        licMsg.style.color='#ffd740';
      }else{
        licMsg.textContent='\u26a0 \u041f\u0440\u043e\u0431\u043d\u044b\u0439 \u043f\u0435\u0440\u0438\u043e\u0434 \u0438\u0441\u0442\u0435\u043a. \u041a\u0443\u043f\u0438\u0442\u0435 \u043b\u0438\u0446\u0435\u043d\u0437\u0438\u044e: wargaelanor@yandex.ru';
        licMsg.style.color='#ff5252';
      }
      $('licenseKey').disabled=false;
    }
  }
  if(d.battery){
    var b=d.battery;
    var cls=b.critical?'batt-crit':b.low?'batt-low':'';
    $('battWrap').className='batt '+cls;
    $('battFill').style.width=b.pct+'%';
    $('battText').textContent=b.mv+'mV ('+b.pct+'%)';
  }
  if(d.sniffer){
    $('snifferCard').style.display='block';
    var html='';
    if(d.sniffer.active){
      $('sniffStartBtn').disabled=true;
      $('sniffStopBtn').disabled=false;
      html+='&#1057;&#1083;&#1091;&#1096;&#1072;&#1102;...<br>';
    }else{
      $('sniffStartBtn').disabled=false;
      $('sniffStopBtn').disabled=true;
    }
    if(d.sniffer.sensors){
      d.sniffer.sensors.forEach(function(s){
        if(s.id){
          html+='ID:'+s.id+' P:'+s.p+'kPa T:'+s.t+'C<br>';
        }
      });
    }
    if(d.sniffer.count>0){
      $('sniffApplyBtn').disabled=false;
      html+='<br>&#1053;&#1072;&#1081;&#1076;&#1077;&#1085;&#1086;: '+d.sniffer.count+' &#1076;&#1072;&#1090;&#1095;&#1080;&#1082;&#1086;&#1074;';
    }
    $('sniffResults').innerHTML=html;
  }
}

/* ======== Load status from REST API ======== */
function loadStatus(retries){
  retries=retries||3;
  var url='/api/status?t='+Date.now();
  fetch(url,{cache:'no-store'}).then(function(r){
    if(!r.ok)throw new Error(r.status);
    return r.json();
  }).then(function(d){
    applyStatus(d);
    var ld=document.querySelector('.loading');
    if(ld)ld.remove();
  }).catch(function(err){
    if(retries>0)setTimeout(function(){loadStatus(retries-1)},800);
    else logMsg('\u041e\u0448\u0438\u0431\u043a\u0430 \u0437\u0430\u0433\u0440\u0443\u0437\u043a\u0438: '+err.message,'err');
  });
}

/* ======== Save sensor ======== */
function saveSensor(i){
  var data={sensor:i,id:$('id'+i).value,pressure:+$('p'+i).value,temp:+$('t'+i).value,enabled:$('e'+i).checked};
  fetch('/api/sensor',{method:'POST',headers:{'Content-Type':'application/json','Cache-Control':'no-cache'},
    body:JSON.stringify(data)})
    .then(function(r){return r.json()}).then(function(j){
      if(j.ok){logMsg('\u0414\u0430\u0442\u0447\u0438\u043a '+(i+1)+' \u0441\u043e\u0445\u0440\u0430\u043d\u0451\u043d','ok');loadStatus(2)}
      else logMsg('\u041e\u0448\u0438\u0431\u043a\u0430 \u0441\u043e\u0445\u0440\u0430\u043d\u0435\u043d\u0438\u044f','err');
    }).catch(function(e){logMsg('\u041e\u0448\u0438\u0431\u043a\u0430: '+e,'err')});
}

/* ======== Send single sensor (WS + REST fallback) ======== */
function txOne(i){
  var btn=$('txbtn'+i);
  var origText=btn.textContent;
  btn.disabled=true;
  btn.textContent='...';
  logMsg('\u041e\u0442\u043f\u0440\u0430\u0432\u043a\u0430 \u0434\u0430\u0442\u0447\u0438\u043a\u0430 '+(i+1)+'...');

  function done(ok){
    btn.disabled=false;
    btn.textContent=origText;
    if(ok){
      btn.classList.add('tx-flash');
      setTimeout(function(){btn.classList.remove('tx-flash')},500);
      logMsg('\u0414\u0430\u0442\u0447\u0438\u043a '+(i+1)+' \u043e\u0442\u043f\u0440\u0430\u0432\u043b\u0435\u043d','ok');
    }else{
      logMsg('\u041e\u0448\u0438\u0431\u043a\u0430 \u043e\u0442\u043f\u0440\u0430\u0432\u043a\u0438 \u0434\u0430\u0442\u0447\u0438\u043a\u0430 '+(i+1),'err');
    }
  }

  if(ws&&ws.readyState===1){
    wsSend({cmd:'tx'+(i+1)});
    done(true);
  }else{
    fetch('/api/cmd',{method:'POST',headers:{'Content-Type':'application/json','Cache-Control':'no-cache'},
      body:JSON.stringify({cmd:'tx'+(i+1)})})
      .then(function(r){return r.json()}).then(function(j){done(j.ok)})
      .catch(function(){done(false)});
  }
}

/* ======== Apply license key ======== */
function applyLicense(){
  var key=$('licenseKey').value.toUpperCase().trim();
  if(key.length!==8){logMsg('\u041a\u043b\u044e\u0447 \u0434\u043e\u043b\u0436\u0435\u043d \u0431\u044b\u0442\u044c 8 \u0441\u0438\u043c\u0432\u043e\u043b\u043e\u0432','err');return}
  fetch('/api/cmd',{method:'POST',headers:{'Content-Type':'application/json','Cache-Control':'no-cache'},
    body:JSON.stringify({cmd:'license',key:key})})
    .then(function(r){return r.json()}).then(function(j){
      if(j.ok){
        logMsg('\u041b\u0438\u0446\u0435\u043d\u0437\u0438\u044f \u0430\u043a\u0442\u0438\u0432\u0438\u0440\u043e\u0432\u0430\u043d\u0430','ok');
        loadStatus(2);
      }else{
        logMsg('\u041d\u0435\u0432\u0435\u0440\u043d\u044b\u0439 \u043a\u043b\u044e\u0447','err');
      }
    }).catch(function(e){logMsg('\u041e\u0448\u0438\u0431\u043a\u0430: '+e,'err')});
}

/* ======== OTA Firmware Update ======== */
function otaUpload(){
  var fileInput=$('otaFile');
  if(!fileInput.files.length){logMsg('\u0412\u044b\u0431\u0435\u0440\u0438\u0442\u0435 .bin \u0444\u0430\u0439\u043b','err');return}
  var file=fileInput.files[0];
  var formData=new FormData();
  formData.append('file',file,file.name);

  var btn=$('otaBtn');
  var prog=$('otaProgress');
  var bar=$('otaBar');
  var status=$('otaStatus');
  var msg=$('otaMsg');

  btn.disabled=true;
  btn.textContent='\u0417\u0430\u0433\u0440\u0443\u0437\u043a\u0430...';
  prog.style.display='block';
  bar.style.width='0%';
  status.textContent='\u041e\u0442\u043f\u0440\u0430\u0432\u043a\u0430... 0%';
  msg.textContent='';

  var xhr=new XMLHttpRequest();
  xhr.open('POST','/api/update',true);

  xhr.upload.onprogress=function(e){
    if(e.lengthComputable){
      var pct=Math.round((e.loaded/e.total)*100);
      bar.style.width=pct+'%';
      status.textContent='\u041e\u0442\u043f\u0440\u0430\u0432\u043a\u0430... '+pct+'%';
    }
  };

  xhr.onload=function(){
    if(xhr.status===200){
      bar.style.width='100%';
      status.textContent='\u0413\u043e\u0442\u043e\u0432\u043e! \u041f\u0435\u0440\u0435\u0437\u0430\u0433\u0440\u0443\u0437\u043a\u0430...';
      logMsg('\u041f\u0440\u043e\u0448\u0438\u0432\u043a\u0430 \u043e\u0431\u043d\u043e\u0432\u043b\u0435\u043d\u0430 \u0443\u0441\u043f\u0435\u0448\u043d\u043e. \u0423\u0441\u0442\u0440\u043e\u0439\u0441\u0442\u0432\u043e \u043f\u0435\u0440\u0435\u0437\u0430\u0433\u0440\u0443\u0436\u0430\u0435\u0442\u0441\u044f...','ok');
    }else{
      status.textContent='\u041e\u0448\u0438\u0431\u043a\u0430: '+xhr.status;
      logMsg('\u041e\u0448\u0438\u0431\u043a\u0430 \u043e\u0431\u043d\u043e\u0432\u043b\u0435\u043d\u0438\u044f','err');
    }
    btn.disabled=false;
    btn.textContent='\u0417\u0430\u0433\u0440\u0443\u0437\u0438\u0442\u044c';
  };

  xhr.onerror=function(){
    status.textContent='\u0421\u0435\u0442\u0435\u0432\u0430\u044f \u043e\u0448\u0438\u0431\u043a\u0430';
    logMsg('\u041e\u0448\u0438\u0431\u043a\u0430 \u0441\u043e\u0435\u0434\u0438\u043d\u0435\u043d\u0438\u044f','err');
    btn.disabled=false;
    btn.textContent='\u0417\u0430\u0433\u0440\u0443\u0437\u0438\u0442\u044c';
  };

  xhr.send(formData);
}

/* ======== Apply auto-TX settings ======== */
function applyAutoTx(){
  var data={interval:+$('atInterval').value,packets:+$('atPackets').value};
  fetch('/api/autotx',{method:'POST',headers:{'Content-Type':'application/json','Cache-Control':'no-cache'},
    body:JSON.stringify(data)})
    .then(function(r){return r.json()}).then(function(j){
      $('atxMsg').textContent=j.ok?'\u0421\u043e\u0445\u0440\u0430\u043d\u0435\u043d\u043e':'\u041e\u0448\u0438\u0431\u043a\u0430';
      setTimeout(function(){$('atxMsg').textContent=''},2000);
    }).catch(function(){});
}

/* ======== Apply frequency ======== */
function applyFreq(){
  var freq=+$('freqSelect').value;
  fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json','Cache-Control':'no-cache'},
    body:JSON.stringify({freq:freq})})
    .then(function(r){return r.json()}).then(function(j){
      if(j.ok){logMsg('\u0427\u0430\u0441\u0442\u043e\u0442\u0430: '+freq+' \u041c\u0413\u0446','ok');loadStatus(2)}
      else logMsg('\u041e\u0448\u0438\u0431\u043a\u0430 \u0443\u0441\u0442\u0430\u043d\u043e\u0432\u043a\u0438 \u0447\u0430\u0441\u0442\u043e\u0442\u044b','err');
    }).catch(function(){});
}

/* ======== Battery life estimate ======== */
function calcBattery(){
  var interval=+$('atInterval').value||300;
  var packets=+$('atPackets').value||2;
  /* 600mAh battery, 7uA sleep, 50mA burst */
  var burstSec=packets*1.0; /* 4 sensors × ~250ms per packet */
  var duty=burstSec/interval;
  var avg=0.007+(50-0.007)*duty;
  var hours=600/avg;
  var days=hours/24;
  var s='~'+days.toFixed(0)+' дн (~'+hours.toFixed(0)+' ч) при 600 мА·ч';
  $('batteryEst').textContent=s;
}

/* ======== TX commands via WS + REST fallback ======== */
function cmdSend(cmd){
  if(cmd==='sniff_start'){
    $('sniffStartBtn').disabled=true;
    $('sniffStopBtn').disabled=false;
    $('sniffApplyBtn').disabled=true;
    $('snifferCard').style.display='block';
    $('sniffResults').innerHTML='&#1057;&#1083;&#1091;&#1096;&#1072;&#1102;...';
  }
  if(cmd==='sniff_stop'){
    $('sniffStartBtn').disabled=false;
    $('sniffStopBtn').disabled=true;
  }
  if(ws&&ws.readyState===1){
    wsSend({cmd:cmd});
  }else{
    fetch('/api/cmd',{method:'POST',headers:{'Content-Type':'application/json','Cache-Control':'no-cache'},
      body:JSON.stringify({cmd:cmd})})
      .then(function(r){return r.json()}).then(function(){loadStatus(2)}).catch(function(){});
  }
}

/* ======== Log ======== */
function logMsg(text,cls){
  var box=$('logBox');
  var d=document.createElement('div');
  if(cls)d.className='log-'+cls;
  d.textContent=text;
  box.appendChild(d);
  if(box.children.length>200)box.removeChild(box.firstChild);
  box.scrollTop=box.scrollHeight;
}

/* ======== Init ======== */
for(var i=0;i<4;i++){
  updateWheel(i,true);
  (function(idx){
    $('e'+idx).addEventListener('change',function(){
      $('el'+idx).textContent=this.checked?'\u0412\u043a\u043b':'\u0412\u044b\u043a\u043b';
      updateWheel(idx,this.checked);
    });
  })(i);
}
$('atInterval').addEventListener('input',calcBattery);
$('atPackets').addEventListener('input',calcBattery);
calcBattery();
loadStatus();
initWs();
</script>
</body>
</html>
)rawliteral";


// ============================================================================
// Serve embedded HTML from PROGMEM (no LittleFS)
// ============================================================================
void handleRoot() {
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "0");
    server.send_P(200, "text/html", INDEX_HTML);
}

// ============================================================================
// HTTP Server Setup
// ============================================================================
void setupWebServer() {
    Serial.println("[HTTP] Setting up web server...");
    server.on("/", handleRoot);
    server.on("/api/status", HTTP_GET, handleApiStatus);
    server.on("/api/sensor", HTTP_POST, handleApiSensor);
    server.on("/api/settings", HTTP_POST, handleApiSettings);
    server.on("/api/autotx", HTTP_POST, handleApiAutoTx);
    server.on("/api/cmd", HTTP_POST, handleApiCmd);
    server.on("/api/update", HTTP_POST, []() {
        // Send response after update completes
        server.sendHeader("Connection", "close");
        if (Update.hasError()) {
            server.send(500, "application/json", "{\"error\":\"update failed\"}");
        } else {
            server.send(200, "application/json", "{\"ok\":true,\"message\":\"restarting\"}");
            delay(500);
            ESP.restart();
        }
    }, handleApiUpdate);
    server.onNotFound([]() {
        Serial.printf("[HTTP] 404: %s\n", server.uri().c_str());
        server.sendHeader("Location", String("http://") + apIP.toString() + "/");
        server.send(302);
    });
    server.begin();
    Serial.println("[HTTP] Web server started");
}

// ============================================================================
// WebSocket Server Setup
// ============================================================================
void setupWebSocket() {
    Serial.println("[WS] Starting WebSocket server...");
    wsServer.begin();
    Serial.println("[WS] WebSocket server started");
    wsServer.onEvent(webSocketEvent);
    Serial.println("[WS] WebSocket event handler registered");
}

// ============================================================================
// WebSocket Helpers
// ============================================================================
void wsSendLog(const String &msg) {
    String json = "{\"t\":\"log\",\"m\":\"";
    String escaped = msg;
    escaped.replace("\\", "\\\\");
    escaped.replace("\"", "\\\"");
    escaped.replace("\n", "\\n");
    json += escaped;
    json += "\"}";
    wsServer.broadcastTXT(json);
}

void wsBroadcastStatus() {
    String json = "{\"t\":\"status\",\"data\":";
    json += buildStatusJson();
    json += "}";
    wsServer.broadcastTXT(json);
}

void wsSendPacketDecode(uint8_t sensorIdx, uint32_t id, uint8_t pByte, uint8_t tByte,
                         uint8_t cnt, uint8_t crc) {
    char idBuf[9];
    snprintf(idBuf, sizeof(idBuf), "%08lx", (unsigned long)id);
    String json = "{\"t\":\"pkt\",\"s\":";
    json += String(sensorIdx);
    json += ",\"id\":\"";
    json += idBuf;
    json += "\",\"p\":";
    json += String((uint16_t)((pByte - PMV107J_PRESSURE_OFFSET) * PMV107J_PRESSURE_KPA_SCALE));
    json += ",\"t\":";
    json += String((int8_t)(tByte - PMV107J_TEMP_OFFSET));
    json += ",\"c\":";
    json += String(cnt);
    json += ",\"crc\":0x";
    if (crc < 0x10) json += "0";
    json += String(crc, HEX);
    json += ",\"ok\":1}";
    wsServer.broadcastTXT(json);
}

// ============================================================================
// Simple JSON Helpers
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
               json[end] != ',' && json[end] != '}' && json[end] != ' ' && json[end] != '\n') {
            end++;
        }
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

// ============================================================================
// Build Status JSON
// ============================================================================
static String buildStatusJson() {
    String json = "{\"sensors\":[";
    for (uint8_t i = 0; i < TPMS_NUM_SENSORS; i++) {
        SensorConfig *s = &config.sensors[i];
        if (i > 0) json += ",";
        bool en = s->flags & 0x01;
        json += "{\"id\":\"";
        char idBuf[9];
        snprintf(idBuf, sizeof(idBuf), "%08lx", (unsigned long)s->sensor_id);
        json += idBuf;
        json += "\",\"pressure\":";
        json += String(s->pressure_kpa);
        json += ",\"temp\":";
        json += String(s->temperature_c);
        json += ",\"enabled\":";
        json += en ? "true" : "false";
        json += "}";
    }
    json += "],\"freq\":";
    json += String(config.freq == 1 ? 433 : 315);
    json += ",\"settings\":{\"datarate\":";
    json += String(config.datarate);
    json += ",\"deviation\":";
    json += String(config.deviation / 10.0f, 1);
    json += ",\"power\":";
    json += String(config.power);
    json += "},\"autoTx\":{\"enabled\":";
    json += autoTxEnabled ? "true" : "false";
    json += ",\"interval\":";
    json += String(autoTxInterval);
    json += ",\"packets\":";
    json += String(autoTxPackets);
    json += "}";
    // Platform info
    json += ",\"platform\":\"";
#if defined(ESP8266)
    json += "esp8266";
#else
    json += "esp32c3";
#endif
    json += "\"";

    // License info
    json += ",\"license\":{\"deviceId\":\"";
    json += get_device_id().c_str();
    json += "\",\"licensed\":";
    json += is_licensed() ? "true" : "false";
    json += ",\"trialSec\":";
    json += String(get_trial_seconds());
    json += ",\"trialMax\":86400";
    json += ",\"email\":\"wargaelanor@yandex.ru\"";
    json += "}";
    // Battery info
    json += ",\"battery\":{\"mv\":";
    json += String(readBatteryMv());
    json += ",\"pct\":";
    json += String(getBatteryPercent());
    json += ",\"low\":";
    json += isBatteryLow() ? "true" : "false";
    json += ",\"critical\":";
    json += isBatteryCritical() ? "true" : "false";
    json += ",\"mah\":";
    json += String(config.battMah);
    json += "}";
    // Sniffer data
    json += ",\"sniffer\":{\"active\":";
    json += snifferActive ? "true" : "false";
    json += ",\"count\":";
    json += String(sniffer_count);
    json += ",\"sensors\":[";
    for (uint8_t i = 0; i < sniffer_count; i++) {
        if (i > 0) json += ",";
        char idBuf[9];
        snprintf(idBuf, sizeof(idBuf), "%08lx", (unsigned long)sniffer_sensors[i].sensor_id);
        json += "{\"id\":\"";
        json += idBuf;
        json += "\",\"p\":";
        json += String(sniffer_sensors[i].pressure_kpa);
        json += ",\"t\":";
        json += String(sniffer_sensors[i].temperature_c);
        json += "}";
    }
    json += "]}}";
    return json;
}

// ============================================================================
// Send Status to one WS client
// ============================================================================
void sendStatusToWs(uint8_t clientNum) {
    String json = "{\"t\":\"status\",\"data\":";
    json += buildStatusJson();
    json += "}";
    wsServer.sendTXT(clientNum, json);
}

// ============================================================================
// WebSocket Event Handler
// ============================================================================
void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            if (wsClients > 0) wsClients--;
            break;

        case WStype_CONNECTED: {
            wsClients++;
            wifiTimeoutMs = millis() + WIFI_CONFIG_TIMEOUT_MS;  // reset timeout
            sendStatusToWs(num);
            break;
        }

        case WStype_TEXT: {
            String msg = String((const char *)payload);
            msg = msg.substring(0, length);

            String cmd = jsonGetString(msg, "cmd");
            if (cmd.length() == 0) { cmd = msg; cmd.trim(); }

            if (cmd == "status") {
                sendStatusToWs(num);
            } else if (cmd == "burst") {
                wsSendLog("[CMD] Burst all enabled sensors");
                send_burst_all();
                wsSendLog("[CMD] Burst done");
                wsBroadcastStatus();
            } else if (cmd == "tx") {
                autoTxEnabled = true;
                config.tx_enabled = 1;
                autoTxLastMs = millis() - (uint32_t)autoTxInterval * 1000UL;
                save_config();
                wsSendLog("[CMD] Auto-TX enabled");
                wsBroadcastStatus();
            } else if (cmd == "stop") {
                autoTxEnabled = false;
                config.tx_enabled = 0;
                save_config();
                wsSendLog("[CMD] Auto-TX disabled");
                wsBroadcastStatus();
            } else if (cmd == "reset") {
                init_config();
                save_config();
                wsSendLog("[CMD] Config reset to defaults");
                wsBroadcastStatus();
            } else if (cmd == "license") {
                String key = jsonGetString(msg, "key");
                if (key.length() > 0) {
                    String devId = get_device_id();
                    if (verify_license(devId.c_str(), key.c_str())) {
                        ee_buf[EE_LICENSE] = 0xFF;
                        EEPROM.write(EE_LICENSE, 0xFF);
                        EEPROM.commit();
                        wsSendLog("[LICENSE] Key accepted — device activated");
                        wsBroadcastStatus();
                    } else {
                        wsSendLog("[LICENSE] Invalid key");
                        wsBroadcastStatus();
                    }
                }
            } else if (cmd == "sniff_start") {
#if !defined(ESP8266)
                sniffer_start();
#endif
                wsBroadcastStatus();
            } else if (cmd == "sniff_stop") {
                sniffer_stop();
                wsBroadcastStatus();
            } else if (cmd == "sniff_apply") {
                for (uint8_t i = 0; i < sniffer_count && i < TPMS_NUM_SENSORS; i++) {
                    config.sensors[i].sensor_id = sniffer_sensors[i].sensor_id;
                    config.sensors[i].pressure_kpa = sniffer_sensors[i].pressure_kpa;
                    config.sensors[i].temperature_c = sniffer_sensors[i].temperature_c;
                    config.sensors[i].battery_ok = sniffer_sensors[i].battery_ok;
                    config.sensors[i].flags = 0x01;
                }
                save_config();
                wsSendLog("[SNIFFER] Applied " + String(sniffer_count) + " sensors to config");
                wsBroadcastStatus();
            } else {
                if (cmd.length() == 3 && cmd.startsWith("tx")) {
                    uint8_t si = cmd.charAt(2) - '1';
                    if (si < TPMS_NUM_SENSORS) {
                        wsSendLog("[CMD] TX sensor " + String(si + 1));
                        send_pmv107j_sensor((uint8_t)si);
                        wsBroadcastStatus();
                    }
                }
            }
            break;
        }

        default:
            break;
    }
}

// ============================================================================
// API: GET /api/status
// ============================================================================
void handleApiStatus() {
    server.sendHeader("Content-Type", "application/json");
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", buildStatusJson());
}

// ============================================================================
// API: POST /api/sensor
// Body: {"sensor":0,"id":"02c038bd","pressure":230,"temp":20,"enabled":true}
// ============================================================================
void handleApiSensor() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"missing body\"}");
        return;
    }
    String body = server.arg("plain");

    int sensorIdx = (int)jsonGetLong(body, "sensor");
    if (sensorIdx < 0 || sensorIdx >= TPMS_NUM_SENSORS) {
        server.send(400, "application/json", "{\"error\":\"sensor index 0-3\"}");
        return;
    }

    String idStr = jsonGetString(body, "id");
    if (idStr.length() > 0) {
        config.sensors[sensorIdx].sensor_id = (uint32_t)strtoul(idStr.c_str(), NULL, 16);
    }
    String pressureStr = jsonGetString(body, "pressure");
    if (pressureStr.length() > 0) {
        config.sensors[sensorIdx].pressure_kpa = (uint8_t)pressureStr.toInt();
    }
    String tempStr = jsonGetString(body, "temp");
    if (tempStr.length() > 0) {
        config.sensors[sensorIdx].temperature_c = (int8_t)tempStr.toInt();
    }
    String enabledStr = jsonGetString(body, "enabled");
    if (enabledStr.length() > 0) {
        bool en = (enabledStr == "true");
        if (en) {
            config.sensors[sensorIdx].flags |= 0x01;
        } else {
            config.sensors[sensorIdx].flags &= ~0x01;
        }
    }

    save_config();
    server.sendHeader("Cache-Control", "no-cache, no-store");
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", "{\"ok\":true}");

    wsSendLog("[API] Sensor " + String(sensorIdx + 1) + " updated");
    wsBroadcastStatus();
}

// ============================================================================
// API: POST /api/settings (CC1101 settings)
// Body: {"datarate":10000,"deviation":38,"power":7}
// ============================================================================
void handleApiSettings() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"missing body\"}");
        return;
    }
    String body = server.arg("plain");

    String rateStr = jsonGetString(body, "datarate");
    if (rateStr.length() > 0) {
        uint32_t rate = (uint32_t)strtoul(rateStr.c_str(), NULL, 10);
        if (!set_cc1101_data_rate(rate)) {
            server.sendHeader("Cache-Control", "no-cache, no-store");
            server.sendHeader("Access-Control-Allow-Origin", "*");
            server.send(400, "application/json", "{\"ok\":false,\"error\":\"data_rate_failed\"}");
            return;
        }
    }

    String devStr = jsonGetString(body, "deviation");
    if (devStr.length() > 0) {
        float dev = devStr.toFloat();
        if (!set_cc1101_deviation(dev)) {
            server.sendHeader("Cache-Control", "no-cache, no-store");
            server.sendHeader("Access-Control-Allow-Origin", "*");
            server.send(400, "application/json", "{\"ok\":false,\"error\":\"deviation_failed\"}");
            return;
        }
    }

    String pwrStr = jsonGetString(body, "power");
    if (pwrStr.length() > 0) {
        int8_t pwr = (int8_t)pwrStr.toInt();
        set_cc1101_power(pwr);
    }

    String freqStr = jsonGetString(body, "freq");
    if (freqStr.length() > 0) {
        int newFreq = freqStr.toInt();
        if (newFreq == 433) {
            config.freq = 1;
        } else {
            config.freq = 0;
        }
        float freq = (config.freq == 1) ? TPMS_FREQ_433 : TPMS_FREQ_315;
        cc1101.setFreq(freq);
        cc1101.setFreqConfig(freq);
        set_cc1101_power(config.power);
        wsSendLog("[API] Frequency changed to " + String(newFreq) + " MHz");
    }

    save_config();
    server.sendHeader("Cache-Control", "no-cache, no-store");
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", "{\"ok\":true}");
    wsSendLog("[API] Settings saved: rate=" + String(config.datarate) +
              " dev=" + String(config.deviation / 10.0f, 1) +
              " pwr=" + String(config.power));
    wsBroadcastStatus();
}

// ============================================================================
// API: POST /api/autotx
// Body: {"enabled":true,"interval":60,"packets":5}
// ============================================================================
void handleApiAutoTx() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"missing body\"}");
        return;
    }
    String body = server.arg("plain");

    String enabledStr = jsonGetString(body, "enabled");
    if (enabledStr.length() > 0) {
        autoTxEnabled = (enabledStr == "true");
    }

    String intervalStr = jsonGetString(body, "interval");
    if (intervalStr.length() > 0) {
        autoTxInterval = (uint16_t)intervalStr.toInt();
        if (autoTxInterval < 5) autoTxInterval = 5;
        if (autoTxInterval > TPMS_MAX_TX_INTERVAL) autoTxInterval = TPMS_MAX_TX_INTERVAL;
    }

    String packetsStr = jsonGetString(body, "packets");
    if (packetsStr.length() > 0) {
        autoTxPackets = (uint8_t)packetsStr.toInt();
        if (autoTxPackets < 1) autoTxPackets = 1;
        if (autoTxPackets > 20) autoTxPackets = 20;
    }

    config.tx_enabled     = autoTxEnabled ? 1 : 0;
    config.autoTxInterval = autoTxInterval;
    config.autoTxPackets  = autoTxPackets;
    save_config();

    server.sendHeader("Cache-Control", "no-cache, no-store");
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", "{\"ok\":true}");
    wsSendLog("[API] Auto-TX: " + String(autoTxEnabled ? "ON" : "OFF") +
              " interval=" + String(autoTxInterval) +
              " packets/sensor=" + String(autoTxPackets));
    wsBroadcastStatus();
}

// ============================================================================
// API: POST /api/cmd
// Body: {"cmd":"burst"} | {"cmd":"tx"} | {"cmd":"stop"} | {"cmd":"reset"} | {"cmd":"tx1"}-{"cmd":"tx4"}
// ============================================================================
void handleApiCmd() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"missing body\"}");
        return;
    }
    String body = server.arg("plain");
    String cmd = jsonGetString(body, "cmd");

    if (cmd == "burst") {
        wsSendLog("[CMD] Burst all enabled sensors");
        send_burst_all();
        wsSendLog("[CMD] Burst done");
    } else if (cmd == "tx") {
        autoTxEnabled = true;
        config.tx_enabled = 1;
        autoTxLastMs = millis() - (uint32_t)autoTxInterval * 1000UL;
        save_config();
        wsSendLog("[CMD] Auto-TX enabled");
    } else if (cmd == "stop") {
        autoTxEnabled = false;
        config.tx_enabled = 0;
        save_config();
        wsSendLog("[CMD] Auto-TX disabled");
    } else if (cmd == "reset") {
        init_config();
        save_config();
        wsSendLog("[CMD] Config reset to defaults");
    } else if (cmd == "license") {
        String key = jsonGetString(body, "key");
        if (key.length() > 0) {
            String devId = get_device_id();
            if (verify_license(devId.c_str(), key.c_str())) {
                ee_buf[EE_LICENSE] = 0xFF;
                EEPROM.write(EE_LICENSE, 0xFF);
                EEPROM.commit();
                wsSendLog("[LICENSE] Key accepted — device activated");
            } else {
                wsSendLog("[LICENSE] Invalid key");
            }
        }
    } else if (cmd == "sniff_start") {
#if !defined(ESP8266)
        sniffer_start();
#endif
    } else if (cmd == "sniff_stop") {
        sniffer_stop();
    } else if (cmd == "sniff_apply") {
        for (uint8_t i = 0; i < sniffer_count && i < TPMS_NUM_SENSORS; i++) {
            config.sensors[i].sensor_id = sniffer_sensors[i].sensor_id;
            config.sensors[i].pressure_kpa = sniffer_sensors[i].pressure_kpa;
            config.sensors[i].temperature_c = sniffer_sensors[i].temperature_c;
            config.sensors[i].battery_ok = sniffer_sensors[i].battery_ok;
            config.sensors[i].flags = 0x01;
        }
        save_config();
        wsSendLog("[SNIFFER] Applied " + String(sniffer_count) + " sensors to config");
    } else if (cmd.length() == 3 && cmd.startsWith("tx")) {
        uint8_t si = cmd.charAt(2) - '1';
        if (si < TPMS_NUM_SENSORS) {
            wsSendLog("[CMD] TX sensor " + String(si + 1));
            send_pmv107j_sensor((uint8_t)si);
        } else {
            server.sendHeader("Cache-Control", "no-cache, no-store");
            server.sendHeader("Access-Control-Allow-Origin", "*");
            server.send(400, "application/json", "{\"error\":\"invalid sensor\"}");
            return;
        }
    } else {
        server.send(400, "application/json", "{\"error\":\"unknown cmd\"}");
        return;
    }

    server.sendHeader("Cache-Control", "no-cache, no-store");
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", "{\"ok\":true}");
    wsBroadcastStatus();
}

// ============================================================================
// OTA Firmware Update Handler
// ============================================================================
static bool otaInProgress = false;
static size_t otaWritten = 0;
static size_t otaTotal = 0;

void handleApiUpdate() {
    HTTPUpload &upload = server.upload();

    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("[OTA] Start: %s\n", upload.filename.c_str());
        otaInProgress = true;
        otaWritten = 0;
        otaTotal = upload.totalSize;

        // Stop sniffer and auto-TX during update
        if (snifferActive) sniffer_stop();
        autoTxEnabled = false;

        // Stop CC1101 to free resources
        cc1101_power_off();

        if (!Update.begin(0)) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Update.printError(Serial);
        }
        otaWritten = upload.currentSize;
    } else if (upload.status == UPLOAD_FILE_END) {
        otaTotal = upload.totalSize;
        Serial.printf("[OTA] End: %u bytes\n", upload.totalSize);
        if (Update.end(true)) {
            Serial.printf("[OTA] Success: %u bytes\n", upload.totalSize);
        } else {
            Update.printError(Serial);
        }
        otaInProgress = false;
    }
    yield();
}

// ============================================================================
// Auto-TX: periodic burst scheduler (called from loop)
// ============================================================================
void runAutoTx() {
    if (!autoTxEnabled || autoTxRunning) return;
    // Block auto-TX when trial expires
    if (!is_licensed() && is_trial_expired()) return;

    uint32_t elapsed = millis() - autoTxLastMs;
    if (elapsed < (uint32_t)autoTxInterval * 1000UL) return;

    autoTxRunning = true;
    Serial.printf("[AUTO-TX] Sending burst (interval=%ds)...\n", autoTxInterval);
    send_burst_all();
    Serial.printf("[AUTO-TX] Burst done\n");
    autoTxRunning = false;
    autoTxLastMs = millis();
}
