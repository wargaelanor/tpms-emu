/*
 * TPMS Emulator/Sniffer Firmware for nRF52840 + CC1101
 * Протокол PMV-107J (Pacific Industrial) для Acura RDX 2008
 * BLE UART интерфейс для Android приложения
 * 
 * Board: ProMicro nRF52840 V1940 (Nice!Nano Feather-совместимый клон)
 * CC1101: 315/433 МГц, 2-FSK, Differential Manchester Encoding
 */

#include <Arduino.h>
#include <SPI.h>
#include <bluefruit.h>

#include "CC1101.h"
#include "flash/flash_nrf5x.h"

extern "C" {
#include "nrf_sdm.h"
#include "nrf_gpio.h"
#include "nrf_soc.h"
}

// =========================================================================
// Pin Mapping - ProMicro nRF52840 V1940
// ВСЕ пины должны быть согласованы во ВСЕХ файлах проекта!
// Note: PIN_SPI_MISO/MOSI/SCK are defined by board variant, we use our own names
// =========================================================================
#define TPMS_PIN_SPI_MISO   29    // Arduino pin 29 = P0.17 (board label "017")
#define TPMS_PIN_SPI_MOSI   20    // Arduino pin 20 = P0.29 (board label "029")
#define TPMS_PIN_SPI_SCK    21    // Arduino pin 21 = P0.31 (board label "031")
#define PIN_CC1101_CS   2     // Arduino pin 2  = P0.10 (board label "010")
#define PIN_CC1101_GDO0 11    // Arduino pin 11 = P0.06 (board label "006")
#define PIN_CC1101_GDO2 12    // Arduino pin 12 = P0.08 (board label "008")
#define PIN_CC1101_PWR  28    // Arduino pin 28 = P0.20 (board label "020")
#define PIN_STATUS_LED  24    // Arduino pin 24 = P0.15 (onboard LED)
#define PIN_BATTERY_ADC 20    // Arduino pin 20 = P0.29 (A6 / PIN_VBAT), делитель 1:2 на B+/VBAT
#define PIN_WAKE_BTN    8     // Arduino pin 8  = P0.05 (board label "005"), wake button

// =========================================================================
// Конфигурация по умолчанию
// =========================================================================
#define CONFIG_MAGIC         0xB1
#define CONFIG_FILENAME      "tpms_cfg"
#define MAX_SENSORS          4
#define MAX_PAYLOAD_BITS     72     // 66 бит payload + preamble
#define DEFAULT_FREQ_315     315
#define DEFAULT_FREQ_433     433
#define DEFAULT_DATARATE     10000  // 10 kbaud
#define DEFAULT_DEVIATION    38000  // 38 kHz
#define DEFAULT_POWER        7      // PA table index 0-7 (7 = 10 dBm max for 315 MHz)
#define DEFAULT_INTERVAL     360    // секунд между burst
#define DEFAULT_PACKETS      2      // пакетов за burst
#define TRIAL_SECONDS        86400  // 24 часа
#define BLE_CHUNK_SIZE       20     // макс. байт за BLE пакет
#define CMD_BUFFER_SIZE      512

// =========================================================================
// Структуры данных (с packed для совместимости хранилища)
// =========================================================================

// Конфигурация одного датчика (7 байт)
struct __attribute__((packed)) SensorConfig {
    uint32_t id;           // 28-bit ID
    uint16_t pressure;     // давление в кПа * 10 (2300 = 230.0 кПа = ~33 psi)
    int8_t   temperature;  // температура в градусах Цельсия
    uint8_t  flags;        // enabled(bit0), counter(bits 1-2)
};

// Полная конфигурация (49 байт)
struct __attribute__((packed)) TPMSConfig {
    uint8_t     magic;          // 0xB0
    uint32_t    datarate;       // скорость в бод
    uint16_t    deviation;      // девиация в Гц / 100
    uint8_t     power;          // PA index
    uint8_t     manch_en;       // Manchester encoding enabled
    uint8_t     tx_enabled;     // авто-TX включён
    uint16_t    tx_interval;    // интервал авто-TX (сек)
    uint8_t     tx_packets;     // пакетов за burst
    uint8_t     freq;           // 315 или 433
    SensorConfig sensors[MAX_SENSORS]; // 4 * 7 = 28 байт
    uint8_t     license_key[4]; // 4 байта лицензии (упрощённо)
    uint32_t    trial_start;    // начало триала (unix timestamp)
    uint16_t    batt_mah;       // ёмкость батареи мАч
    uint8_t     batt_pin;       // аналоговый пин батареи (Arduino-номер), 0/255 = авто A1
    uint16_t    batt_cal_raw;   // сырой ADC при калибровке (0 = нет калибровки)
    uint16_t    batt_cal_mv;    // реальное напряжение при калибровке (мВ)
};

// Данные обнаруженного датчика (для сниффера)
struct DiscoveredSensor {
    uint32_t id;
    uint16_t pressure;
    int8_t   temperature;
    uint8_t  counter;
    uint8_t  bat_flag;
    int8_t   rssi;
    bool     valid;
};

// =========================================================================
// Глобальные объекты
// =========================================================================
static CC1101 radio(PIN_CC1101_CS, PIN_CC1101_GDO0, PIN_CC1101_GDO2);
static TPMSConfig cfg;
static DiscoveredSensor discovered[MAX_SENSORS];

// Flash config storage - page 200 (0xC8000), between app and bootloader
#define CONFIG_FLASH_PAGE  200
#define CONFIG_FLASH_ADDR  (CONFIG_FLASH_PAGE * 4096)
#define CONFIG_FLASH_MAGIC 0x54504D43  // "TPMC"

typedef struct {
    uint32_t magic;
    TPMSConfig cfg;
    uint32_t checksum;
} FlashConfig;

// BLE
static BLEDis  bledis;
static BLEUart bleuart;

// Буфер команд
static char cmdBuf[CMD_BUFFER_SIZE];
static volatile uint16_t cmdBufLen = 0;

// Состояние
static bool snifferActive = false;
static uint32_t lastAutoTx = 0;
static uint32_t trialStartTime = 0;
static uint32_t licenseValid = 0; // 0 = no license, 1 = trial, 2 = full
static volatile bool configDirty = false;

// BLE power management
static bool bleConnected = false;
static bool bleAdvertising = true;
static bool bleHadConnection = false;  // true после первого реального connect
static uint32_t bleStartTime = 0;
#define BLE_ADVERTISE_TIMEOUT_MS  0xFFFFFFFFu  // анонсируемся бесконечно, пока нет подключения

static void cc1101Sleep();

static void enterDeepSleep() {
    cc1101Sleep();
    digitalWrite(PIN_STATUS_LED, LOW);
    
    // Stop BLE advertising (but keep SoftDevice alive for sd_power_system_off!)
    Bluefruit.Advertising.stop();
    
    // Configure wake pin: button to GND, wake on LOW
    pinMode(PIN_WAKE_BTN, INPUT_PULLUP_SENSE);
    
    // Disable SD event IRQ to prevent spurious wake
    NVIC_DisableIRQ(SD_EVT_IRQn);
    
    // Enter System OFF via SoftDevice SVC (SD must be enabled!)
    sd_power_system_off();
    
    while(1) { __WFE(); }
}

// Forward declarations
static bool saveConfig();
static void sniffer_stop();
static void cc1101Sleep();

// =========================================================================
// Битовые хелперы
// =========================================================================
static inline void set_bit(uint8_t *arr, uint16_t bit, uint8_t val) {
    uint16_t idx = bit >> 3;
    uint8_t  mask = 1 << (7 - (bit & 7));
    if (val) arr[idx] |= mask; else arr[idx] &= ~mask;
}

static inline uint8_t get_bit(const uint8_t *arr, uint16_t bit) {
    uint16_t idx = bit >> 3;
    return (arr[idx] >> (7 - (bit & 7))) & 1;
}

// =========================================================================
// Battery monitoring
// =========================================================================
// Эта плата (SuperMini/ProMicro NRF52840, клон nice!nano V2) НЕ имеет
// распаянного делителя батареи на GPIO-пинах («Voltage divider (0.24) is
// unpopulated», см. https://github.com/joric/nrfmicro/wiki/Alternatives#supermini-nrf52840).
// Напряжение батареи присутствует на шине VDDH (через зарядную микросхему),
// поэтому по умолчанию измеряем его внутренним SAADC-каналом VDDH/5
// (analogReadVDDHDIV5). Для редких плат с распаянным делителем можно явно
// указать batt_pin (14..21) — тогда читаем внешний пина.
static int readBatteryRaw() {
    analogReadResolution(10);
    if (cfg.batt_pin >= A0 && cfg.batt_pin <= A7) {
        return analogRead(cfg.batt_pin);
    }
    return (int)analogReadVDDHDIV5();  // внутренний канал VDDH/5 (по умолчанию)
}

// USB-детект: VBUS присутствует через USBREGSTATUS (nRF52840). У этой платы
// при USB-питании шина VDDH подтягивается к шине USB (~4.2 В), поэтому показания
// батареи недостоверны — нужно явно отличать USB от реальной АКБ.
static bool isUsbPowered() {
    return (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk)
           == POWER_USBREGSTATUS_VBUSDETECT_VbusPresent;
}

static uint16_t readBatteryMv() {
    // USB-питание: VDDH = шина USB, батарею не измеряем → 0
    if (isUsbPowered()) return 0;
    int adc = readBatteryRaw();
    if (cfg.batt_cal_raw > 0 && cfg.batt_cal_mv > 0) {
        uint32_t mv = (uint32_t)adc * cfg.batt_cal_mv / cfg.batt_cal_raw;
        return (uint16_t)mv;
    }
    // Канал VDDH/5: полная шкала 10 бит (1023) = 3.6 В на входе SAADC = 18 В на VDDH.
    // мВ = raw * 3600 * 5 / 1023 = raw * 18000 / 1023.
    uint32_t mv = (uint32_t)adc * 18000UL / 1023UL;
    // VDDH выше 4.6 В означает USB-питание без батареи (у этой платы VDDH
    // переключается на 5V). Чтобы не показывать «заряжено» при отключённой
    // батарее, возвращаем 0.
    if (mv > 4600) return 0;
    return (uint16_t)mv;
}

static uint8_t getBatteryPercent() {
    uint16_t mv = readBatteryMv();
    // Линейная интерполяция: 3000mV = 0%, 4200mV = 100%
    if (mv <= 3000) return 0;
    if (mv >= 4200) return 100;
    return (uint8_t)((mv - 3000) * 100 / 1200);
}

static bool isBatteryLow() {
    return readBatteryMv() < 3300;
}

static bool isBatteryCritical() {
    return readBatteryMv() < 3100;
}

// =========================================================================
// CC1101 Power Control
// =========================================================================
static void cc1101PowerOn() {
    pinMode(PIN_CC1101_PWR, OUTPUT);
    digitalWrite(PIN_CC1101_PWR, HIGH);
    delay(10);  // ждём стабилизации питания CC1101
}

static void cc1101PowerOff() {
    pinMode(PIN_CC1101_PWR, OUTPUT);
    digitalWrite(PIN_CC1101_PWR, LOW);
}

static void cc1101Sleep() {
    // Temporarily disabled for TX debugging — replicating old behavior
    // where CC1101 stays powered and configured after setup()
    radio.sendCommand(CC1101_SIDLE);  // just go to IDLE
}

static void cc1101Wake() {
    // No-op — CC1101 stays powered and configured from setup()
}

// =========================================================================
// BLE helpers - chunked write для избежания FIFO overflow
// =========================================================================
static void bleSendChunked(const char *str) {
    if (!bleConnected) return;  // don't send if not connected
    uint16_t len = strlen(str);
    uint16_t offset = 0;
    while (offset < len) {
        uint8_t chunk = (len - offset > BLE_CHUNK_SIZE) ? BLE_CHUNK_SIZE : (len - offset);
        bleuart.write((uint8_t *)(str + offset), chunk);
        offset += chunk;
        // Небольшая пауза между чанками
        if (offset < len) delay(5);
    }
    bleuart.write((uint8_t *)"\n", 1);
}

static void sendLog(const char *msg) {
    char buf[280];
    snprintf(buf, sizeof(buf), "{\"t\":\"log\",\"m\":\"%s\"}", msg);
    bleSendChunked(buf);
}

static uint32_t calcFlashChecksum(const TPMSConfig *c) {
    uint32_t sum = 0;
    const uint32_t *p = (const uint32_t *)c;
    for (size_t i = 0; i < sizeof(TPMSConfig) / 4; i++) {
        sum ^= p[i];
    }
    return sum;
}

static bool saveConfig() {
    cfg.trial_start = trialStartTime;

    FlashConfig fc;
    fc.magic = CONFIG_FLASH_MAGIC;
    fc.cfg = cfg;
    fc.checksum = calcFlashChecksum(&cfg);

    if (!flash_nrf5x_erase(CONFIG_FLASH_ADDR)) {
        sendLog("Flash erase FAILED");
        return false;
    }

    int written = flash_nrf5x_write(CONFIG_FLASH_ADDR, &fc, sizeof(FlashConfig));
    if (written != (int)sizeof(FlashConfig)) {
        sendLog("Flash write FAILED");
        return false;
    }

    // flash_nrf5x_write() только буферизует данные в кэше библиотеки.
    // Реальное программирование флэша происходит только при flush/переходе на другую страницу.
    // Без flush() конфиг теряется при перезагрузке, поэтому обязателен вызов flash_nrf5x_flush().
    flash_nrf5x_flush();
    sendLog("Config saved to flash");

    return true;
}

// =========================================================================
// CRC-8 для PMV-107J: полином 0x13, init 0x00
// =========================================================================
static uint8_t calculate_crc8_pmv(const uint8_t *data, uint8_t len) {
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

// =========================================================================
// PMV-107J: построение 66-битного payload
// Matching rtl_433 decoder (tpms_pmv107j.c):
//   Bits 0-27:  ID (28 bits, MSB first)
//   Bits 28-33: Status (6 bits: battery_low, counter[1:0], unknown=0, rapid_change, failed)
//   Bits 34-41: Pressure (8 bits = kPa/2.48 + 40)
//   Bits 42-49: Inverted pressure (8 bits = pressure XOR 0xFF)
//   Bits 50-57: Temperature (8 bits = Celsius + 40)
//   Bits 58-65: CRC-8 (8 bits, poly 0x13, init 0)
// =========================================================================
static uint16_t build_pmv107j_payload(uint8_t *outBits, uint32_t sensorId,
                                       uint16_t pressure, int8_t temperature,
                                       uint8_t counter, uint8_t batFlag) {
    uint16_t payloadBits = 66;
    uint16_t arrLen = (payloadBits + 7) / 8;
    memset(outBits, 0, arrLen);

    // Encode pressure: stored as kPa*10 (2300=230.0kPa) -> rtl_433 format (kPa/2.48 + 40)
    uint8_t pressureEnc = 0;
    if (pressure > 0) {
        float pKpa = (float)pressure / 10.0f;
        int32_t pVal = (int32_t)(pKpa / 2.48f) + 40;
        if (pVal < 0) pVal = 0;
        if (pVal > 255) pVal = 255;
        pressureEnc = (uint8_t)pVal;
    }

    // Encode temperature: Celsius + 40
    int32_t tVal = (int32_t)temperature + 40;
    if (tVal < 0) tVal = 0;
    if (tVal > 255) tVal = 255;
    uint8_t tempEnc = (uint8_t)tVal;

    // ID: 28 bits, MSB first (matching rtl_433's b[0]<<26 | b[1]<<18 | ...)
    for (int i = 0; i < 28; i++) {
        set_bit(outBits, i, (sensorId >> (27 - i)) & 1);
    }

    // Status/Flags: 6 bits (bits 28-33)
    // rtl_433: battery_low(bit5), counter[1:0](bits4:3), unknown(bit2)=0, rapid_change(bit1)=0, failed(bit0)=0
    set_bit(outBits, 28, batFlag);                    // battery_low
    set_bit(outBits, 29, (counter >> 1) & 1);        // counter bit 1
    set_bit(outBits, 30, (counter >> 0) & 1);        // counter bit 0
    set_bit(outBits, 31, 0);                          // unknown (must be 0)
    set_bit(outBits, 32, 0);                          // rapid_change
    set_bit(outBits, 33, 0);                          // failed

    // Pressure direct: 8 bits (bits 34-41), MSB first
    for (int i = 0; i < 8; i++) {
        set_bit(outBits, 34 + i, (pressureEnc >> (7 - i)) & 1);
    }

    // Pressure inverted: 8 bits (bits 42-49), MSB first = pressureEnc XOR 0xFF
    uint8_t pressureInv = pressureEnc ^ 0xFF;
    for (int i = 0; i < 8; i++) {
        set_bit(outBits, 42 + i, (pressureInv >> (7 - i)) & 1);
    }

    // Temperature: 8 bits (bits 50-57), MSB first
    for (int i = 0; i < 8; i++) {
        set_bit(outBits, 50 + i, (tempEnc >> (7 - i)) & 1);
    }

    // CRC-8 over 64 bits (6 prepended zeros + 58 payload bits)
    // Build the same byte layout as rtl_433's b[0..7]:
    // b[0] = 0b000000XX (6 zeros + ID[27:26])
    // b[1..7] = remaining 56 bits
    uint8_t crcBytes[8];
    memset(crcBytes, 0, sizeof(crcBytes));
    // Pack all 66 payload bits into crcBytes, prepending 6 zeros
    for (int i = 0; i < 64; i++) {
        int srcBit = i - 6;  // source bit in outBits (-6 to 57)
        uint8_t bitVal = 0;
        if (srcBit >= 0 && srcBit < 58) {
            bitVal = get_bit(outBits, srcBit);
        }
        if (bitVal) {
            crcBytes[i / 8] |= (1 << (7 - (i % 8)));
        }
    }
    // Compute CRC-8 with polynomial 0x13, init 0
    uint8_t crcVal = calculate_crc8_pmv(crcBytes, 8);

    // CRC in bits 58-65
    for (int i = 0; i < 8; i++) {
        set_bit(outBits, 58 + i, (crcVal >> (7 - i)) & 1);
    }

    return payloadBits;
}

// =========================================================================
// Differential Manchester Encoding
// Правило: бит 0 = переход, бит 1 = без перехода
// Первый символ всегда = 1 (начало preamble)
// =========================================================================
static uint16_t differential_manchester_encode(const uint8_t *inBits, uint16_t inLen,
                                                uint8_t *outBits) {
    uint16_t outLen = inLen * 2;
    uint16_t arrLen = (outLen + 7) / 8;
    memset(outBits, 0, arrLen);

    uint16_t outPos = 0;
    uint8_t state = 0;

    for (uint16_t i = 0; i < inLen; i++) {
        uint8_t bit = get_bit(inBits, i);
        uint8_t same = (bit == state);

        if (same) {
            set_bit(outBits, outPos++, 1);
            set_bit(outBits, outPos++, 0);
            state = 0;
        } else {
            set_bit(outBits, outPos++, 0);
            set_bit(outBits, outPos++, 1);
            state = 1;
        }
    }

    return outPos;
}

// =========================================================================
// PMV-107J: построение полного пакета
// Рабочий формат (как в TPMS-Emulator):
//   16 settle zeros (raw) + "111110" preamble (raw, 6 bits)
//   + DM-encoded payload (1 + 66 + 1 = 68 bits → 136 DM bits)
//   + 6 trailer zeros (raw)
//   Итого: 16 + 6 + 136 + 6 = 164 bits = 21 bytes
// =========================================================================
static uint16_t build_pmv107j_packet(uint8_t *packet, uint32_t sensorId,
                                      uint16_t pressure, int8_t temperature,
                                      uint8_t counter, uint8_t batFlag) {
    // Step 1: Build 66-bit payload (ID + flags + pressure + inv_pressure + temp + CRC)
    uint8_t payload_bits[16];
    memset(payload_bits, 0, sizeof(payload_bits));
    build_pmv107j_payload(payload_bits, sensorId, pressure, temperature, counter, batFlag);

    // Step 2: Build DM input: '1' + 66 payload bits + '1' = 68 bits
    uint8_t dm_input[16];
    memset(dm_input, 0, sizeof(dm_input));
    uint16_t dm_in_pos = 0;
    set_bit(dm_input, dm_in_pos++, 1);
    for (uint16_t i = 0; i < 66; i++) {
        set_bit(dm_input, dm_in_pos++, get_bit(payload_bits, i));
    }
    set_bit(dm_input, dm_in_pos++, 1);

    // Step 3: Differential Manchester encode
    uint8_t dm_output[40];
    memset(dm_output, 0, sizeof(dm_output));
    uint16_t dm_out_len = differential_manchester_encode(dm_input, dm_in_pos, dm_output);

    // Step 4: Build complete on-air bitstream
    // 16 settle zeros + "111110" + DM data + 6 trailer zeros
    uint8_t tx_buf[30];
    memset(tx_buf, 0, sizeof(tx_buf));
    uint16_t tx_pos = 0;

    tx_pos = 16;  // SETTLE_BITS: 16 zeros (already zero from memset)

    // Preamble: "111110" (6 bits: 5 ones + 1 zero)
    for (uint8_t i = 0; i < 6; i++) {
        if (i < 5) set_bit(tx_buf, tx_pos + i, 1);
    }
    tx_pos += 6;

    // DM-encoded payload
    for (uint16_t i = 0; i < dm_out_len; i++) {
        set_bit(tx_buf, tx_pos + i, get_bit(dm_output, i));
    }
    tx_pos += dm_out_len;

    // Trailer: 6 zeros (already zero from memset)
    tx_pos += 6;

    uint8_t tx_bytes = (tx_pos + 7) / 8;
    memcpy(packet, tx_buf, tx_bytes);
    return tx_bytes;
}

// =========================================================================
// Отправка сырых данных через CC1101 (fixed-length TX)
// =========================================================================
static void cc1101_send_raw(const uint8_t *data, uint8_t len) {
    cc1101Wake();

    radio.setIdleState();
    delay(1);

    radio.sendCommand(CC1101_SCAL);
    delay(3);

    // Fixed length TX mode (matching working TPMS-Emulator project)
    radio.writeReg(CC1101_PKTCTRL0, 0x00);
    radio.writeReg(CC1101_PKTLEN, len);
    radio.writeReg(CC1101_PKTCTRL1, 0x00);
    radio.setManc(0);

    radio.flushTxFifo();
    delay(1);
    radio.writeBurstReg(CC1101_TXFIFO, data, len);

    // Старт TX
    radio.sendCommand(CC1101_STX);

    // Ждём окончания передачи
    delay(3);
    unsigned long start = millis();
    while (millis() - start < 200) {
        uint8_t state = radio.getMarcState();
        if (state == CC1101_MARCSTATE_IDLE || state == 0x01) break;
        delayMicroseconds(100);
    }

    radio.setIdleState();
    delay(1);
    radio.flushTxFifo();
}

// =========================================================================
// Отправка пакета PMV-107J для одного датчика
// =========================================================================
static void send_pmv107j_sensor(uint8_t sensorIdx) {
    if (sensorIdx >= MAX_SENSORS) return;
    if (!(cfg.sensors[sensorIdx].flags & 0x01)) return; // disabled

    SensorConfig *s = &cfg.sensors[sensorIdx];
    uint8_t counter = (s->flags >> 1) & 0x03;
    uint8_t batFlag = isBatteryLow() ? 1 : 0;

    uint8_t packet[32];
    uint8_t pktLen = build_pmv107j_packet(packet, s->id, s->pressure,
                                            s->temperature, counter, batFlag);

    // Отправляем пакет
    cc1101_send_raw(packet, pktLen);

    // Инкрементируем счётчик
    counter = (counter + 1) & 0x03;
    s->flags = (s->flags & 0xF9) | (counter << 1);

    // Посылаем информацию о пакете в BLE
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"t\":\"pkt\",\"s\":%d,\"id\":\"%08X\",\"p\":%d,\"tmp\":%d,\"c\":%d,\"crc\":\"OK\"}",
             sensorIdx, (unsigned)s->id, (unsigned)s->pressure / 10, (int)s->temperature, counter);
    bleSendChunked(buf);
}

// =========================================================================
// Burst: отправка всех включённых датчиков
// =========================================================================
static void sendBurst() {
    sendLog("Burst start");
    for (uint8_t i = 0; i < MAX_SENSORS; i++) {
        if (cfg.sensors[i].flags & 0x01) {
            send_pmv107j_sensor(i);
            delay(300);  // пауза между датчиками
        }
    }
    sendLog("Burst done");
}

// =========================================================================
// Differential Manchester Decode (для сниффера)
// =========================================================================
static int16_t dm_decode(const uint8_t *inBits, uint16_t inLen, uint8_t *outBits) {
    // DM decode: каждые 2 бита → 1 бит
    // 0: переход, 1: без перехода
    uint16_t outLen = inLen / 2;
    memset(outBits, 0, (outLen + 7) / 8);

    uint8_t prevLevel = 1;
    uint16_t outIdx = 0;

    for (uint16_t i = 0; i + 1 < inLen; i += 2) {
        uint8_t a = get_bit(inBits, i);
        uint8_t b = get_bit(inBits, i + 1);

        if (a != b) {
            // Переход → бит 0
            set_bit(outBits, outIdx++, 0);
            prevLevel = b;
        } else {
            // Без перехода → бит 1
            set_bit(outBits, outIdx++, 1);
            prevLevel = a;
        }
    }

    return outIdx;
}

// =========================================================================
// Декодер пакетов PMV-107J (сниффер)
// =========================================================================
static bool sniffer_decode_packet(const uint8_t *data, uint8_t len,
                                   DiscoveredSensor *result) {
    // Конвертируем байты в биты
    uint16_t totalBits = len * 8;
    uint8_t *rawBits = (uint8_t *)malloc((totalBits + 7) / 8);
    if (!rawBits) return false;
    memcpy(rawBits, data, len);

    // DM decode
    uint8_t *dmBits = (uint8_t *)malloc((totalBits / 2 + 7) / 8);
    if (!dmBits) { free(rawBits); return false; }

    int16_t decodedLen = dm_decode(rawBits, totalBits, dmBits);
    free(rawBits);

    if (decodedLen < 88) {  // минимум preamble(16) + sync(6) + payload(66) = 88
        free(dmBits);
        return false;
    }

    // Ищем preamble: 16 нулей + "111110"
    int syncPos = -1;
    for (int i = 0; i <= decodedLen - 22; i++) {
        // Проверяем 16 нулей
        bool preambleOk = true;
        for (int j = 0; j < 16 && (i + j) < decodedLen; j++) {
            if (get_bit(dmBits, i + j) != 0) { preambleOk = false; break; }
        }
        if (!preambleOk) continue;

        // Проверяем sync "111110"
        uint8_t syncCheck[] = {1, 1, 1, 1, 1, 0};
        bool syncOk = true;
        for (int j = 0; j < 6 && (i + 16 + j) < decodedLen; j++) {
            if (get_bit(dmBits, i + 16 + j) != syncCheck[j]) { syncOk = false; break; }
        }
        if (syncOk) { syncPos = i; break; }
    }

    if (syncPos < 0) { free(dmBits); return false; }

    // Извлекаем payload начиная с позиции syncPos + 22
    uint16_t payloadStart = syncPos + 22;
    if (payloadStart + 66 > decodedLen) { free(dmBits); return false; }

    // ID: 28 бит, LSB first
    uint32_t id = 0;
    for (int i = 0; i < 28; i++) {
        if (get_bit(dmBits, payloadStart + i)) {
            id |= (1UL << i);
        }
    }

    // Battery flag
    uint8_t batFlag = get_bit(dmBits, payloadStart + 28);

    // Counter: 2 бита
    uint8_t counter = get_bit(dmBits, payloadStart + 29) |
                     (get_bit(dmBits, payloadStart + 30) << 1);

    // Pressure inverted: 11 бит, LSB first
    uint16_t presInv = 0;
    for (int i = 0; i < 11; i++) {
        if (get_bit(dmBits, payloadStart + 33 + i)) {
            presInv |= (1 << i);
        }
    }

    // Pressure direct: 11 бит, LSB first
    uint16_t presDirect = 0;
    for (int i = 0; i < 11; i++) {
        if (get_bit(dmBits, payloadStart + 44 + i)) {
            presDirect |= (1 << i);
        }
    }

    // Проверяем: presDirect должен быть ~ 4095 - presInv
    uint16_t pressure = presDirect;
    if (presInv != (4095 - presDirect)) {
        // Попробуем инвертированное
        pressure = 4095 - presInv;
    }

    // Temperature: 3 бита, XOR 0x07
    uint8_t tempEnc = get_bit(dmBits, payloadStart + 55) |
                     (get_bit(dmBits, payloadStart + 56) << 1) |
                     (get_bit(dmBits, payloadStart + 57) << 2);
    int8_t temperature = (int8_t)((tempEnc ^ 0x07) * 10 - 40);

    // CRC check
    uint8_t crcCalc = 0;
    uint8_t crcRx = 0;
    // Собираем первые 58 бит payload
    uint8_t payload58[8];
    memset(payload58, 0, sizeof(payload58));
    for (int i = 0; i < 58; i++) {
        if (get_bit(dmBits, payloadStart + i)) {
            payload58[i / 8] |= (1 << (7 - (i % 8)));
        }
    }
    crcCalc = calculate_crc8_pmv(payload58, 8);

    for (int i = 0; i < 8; i++) {
        if (get_bit(dmBits, payloadStart + 58 + i)) {
            crcRx |= (1 << i);
        }
    }

    free(dmBits);

    // Заполняем результат
    result->id = id;
    result->pressure = pressure;
    result->temperature = temperature;
    result->counter = counter;
    result->bat_flag = batFlag;
    result->rssi = radio.getRssi();
    result->valid = (crcCalc == crcRx);

    return result->valid;
}

// =========================================================================
// Sniffer: управление
// =========================================================================
static void sniffer_start() {
    cc1101Wake();
    radio.setIdleState();
    radio.flushRxFifo();

    // Настраиваем для приёма
    radio.writeReg(CC1101_PKTCTRL0, 0x02);  // Infinite length, no CRC, normal FIFO
    radio.writeReg(CC1101_PKTLEN, 0xFF);     // максимальная длина
    radio.writeReg(CC1101_MDMCFG2, 0x10);   // 2-FSK, no Manchester, no sync
    radio.writeReg(CC1101_IOCFG0, 0x06);    // GDO0 = sync detect

    radio.setRxState();
    snifferActive = true;

    for (int i = 0; i < MAX_SENSORS; i++) {
        discovered[i].valid = false;
    }

    sendLog("Sniffer started");
}

static void sniffer_stop() {
    snifferActive = false;

    int applied = 0;
    for (int i = 0; i < MAX_SENSORS; i++) {
        if (discovered[i].valid) {
            cfg.sensors[i].id = discovered[i].id;
            cfg.sensors[i].pressure = discovered[i].pressure;
            cfg.sensors[i].temperature = discovered[i].temperature;
            cfg.sensors[i].flags = 0x01;
            applied++;
        }
    }
    if (applied > 0) {
        saveConfig();
        char logBuf[64];
        snprintf(logBuf, sizeof(logBuf), "Auto-applied %d discovered sensors", applied);
        sendLog(logBuf);
    }

    cc1101Sleep();
    sendLog("Sniffer stopped");
}

static void sniffer_loop() {
    if (!snifferActive) return;

    uint8_t rxBytes = radio.getRxBytes();
    if (rxBytes == 0 || (rxBytes & 0x80)) {
        if (rxBytes & 0x80) radio.flushRxFifo();
        return;
    }

    uint8_t len = rxBytes & 0x7F;
    if (len < 10 || len > 30) {
        radio.flushRxFifo();
        return;
    }

    uint8_t buf[32];
    radio.readBurstReg(CC1101_RXFIFO, buf, len);
    radio.flushRxFifo();

    // Проверяем, что в RX режиме
    uint8_t state = radio.getMarcState();
    if (state != CC1101_MARCSTATE_RX) {
        radio.setRxState();
    }

    DiscoveredSensor ds;
    if (sniffer_decode_packet(buf, len, &ds)) {
        // Проверяем, не нашли ли уже этот ID
        int slot = -1;
        for (int i = 0; i < MAX_SENSORS; i++) {
            if (discovered[i].valid && discovered[i].id == ds.id) {
                // Обновляем данные
                discovered[i] = ds;
                slot = i;
                break;
            }
            if (!discovered[i].valid && slot < 0) {
                slot = i;
            }
        }

        if (slot >= 0 && !discovered[slot].valid) {
            // Новый датчик
            discovered[slot] = ds;
        }

        // Отправляем данные в BLE
        char buf2[160];
        snprintf(buf2, sizeof(buf2),
                 "{\"t\":\"pkt\",\"id\":\"%08X\",\"p\":%d,\"tmp\":%d,\"c\":%d,\"rssi\":%d}",
                 (unsigned)ds.id, (unsigned)ds.pressure, (int)ds.temperature,
                 (int)ds.counter, (int)ds.rssi);
        bleSendChunked(buf2);
    }
}

// =========================================================================
// Конфигурация: загрузка/сохранение через LittleFS
// =========================================================================
static void setDefaultConfig() {
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = CONFIG_MAGIC;
    cfg.datarate = DEFAULT_DATARATE;
    cfg.deviation = DEFAULT_DEVIATION / 100;
    cfg.power = DEFAULT_POWER;
    cfg.manch_en = 1;
    cfg.tx_enabled = 1;
    cfg.tx_interval = DEFAULT_INTERVAL;
    cfg.tx_packets = DEFAULT_PACKETS;
    cfg.freq = DEFAULT_FREQ_315;
    cfg.batt_mah = 500;
    cfg.batt_pin = 0xFF;  // 0xFF = внутренний канал VDDH/5 (по умолчанию для этой платы)
    cfg.batt_cal_raw = 0;  // калибровка не выполнена
    cfg.batt_cal_mv = 0;

    // Датчики по умолчанию (Acura RDX 2008)
    cfg.sensors[0].id = 0x00298088; cfg.sensors[0].pressure = 2300; cfg.sensors[0].temperature = 20; cfg.sensors[0].flags = 0x01;
    cfg.sensors[1].id = 0x0466E088; cfg.sensors[1].pressure = 2300; cfg.sensors[1].temperature = 20; cfg.sensors[1].flags = 0x01;
    cfg.sensors[2].id = 0x0D784088; cfg.sensors[2].pressure = 2300; cfg.sensors[2].temperature = 20; cfg.sensors[2].flags = 0x01;
    cfg.sensors[3].id = 0x0C765088; cfg.sensors[3].pressure = 2300; cfg.sensors[3].temperature = 20; cfg.sensors[3].flags = 0x01;

    trialStartTime = millis() / 1000;
}

static bool loadConfig() {
    FlashConfig *fc = (FlashConfig *)CONFIG_FLASH_ADDR;

    if (fc->magic != CONFIG_FLASH_MAGIC) {
        sendLog("No config in flash, using defaults");
        setDefaultConfig();
        return false;
    }

    uint32_t expected = calcFlashChecksum(&fc->cfg);
    if (expected != fc->checksum) {
        sendLog("Config checksum bad, using defaults");
        setDefaultConfig();
        return false;
    }

    memcpy(&cfg, &fc->cfg, sizeof(cfg));
    trialStartTime = cfg.trial_start;

    // Валидация batt_pin: старые конфиги, сохранённые до добавления этого поля,
    // содержат мусор. Допустимы 0xFF (внутренний VDDH/5) или аналоговые пины A0-A7 (Arduino 14-21).
    {
        bool validPin = (cfg.batt_pin == 0xFF);
        for (uint8_t i = 0; i < 8; i++) {
            if (cfg.batt_pin == (A0 + i)) { validPin = true; break; }
        }
        if (!validPin) {
            cfg.batt_pin = 0xFF;  // дефолт: внутренний канал VDDH/5
        }
    }

    if (cfg.license_key[0] || cfg.license_key[1] || cfg.license_key[2] || cfg.license_key[3]) {
        licenseValid = 2;
    } else if (trialStartTime > 0) {
        uint32_t elapsed = (millis() / 1000) - trialStartTime;
        licenseValid = (elapsed < TRIAL_SECONDS) ? 1 : 0;
    }

    sendLog("Config loaded from flash");
    return true;
}

// =========================================================================
// Отправка полного статуса в BLE
// =========================================================================
static void sendStatus() {
    char buf[768];
    int pos = 0;

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "{\"t\":\"status\",\"data\":{");

    // Настройки
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "\"freq\":%d,\"drate\":%lu,\"dev\":%u,\"pwr\":%u,"
        "\"tx_en\":%d,\"tx_int\":%u,\"tx_pkt\":%u,"
"\"batt_mv\":%u,\"batt_pct\":%u,\"batt_raw\":%d,\"batt_pin\":%d,\"batt_cal\":%u,\"license\":%u,"
        "\"sniff\":%d,\"usb\":%d,",
        cfg.freq, (unsigned long)cfg.datarate, (unsigned)cfg.deviation, (unsigned)cfg.power,
        cfg.tx_enabled, (unsigned)cfg.tx_interval, (unsigned)cfg.tx_packets,
        (unsigned)readBatteryMv(), (unsigned)getBatteryPercent(), readBatteryRaw(),
        (int)cfg.batt_pin,
        cfg.batt_cal_raw > 0 ? cfg.batt_cal_mv : 0,
        (unsigned)licenseValid, snifferActive ? 1 : 0, isUsbPowered() ? 1 : 0);

    // Датчики
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\"sensors\":[");
    const char *labels[] = {"PL", "PP", "ZL", "ZP"};
    for (int i = 0; i < MAX_SENSORS; i++) {
        SensorConfig *s = &cfg.sensors[i];
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"i\":%d,\"id\":\"%08X\",\"p\":%u,\"t\":%d,\"en\":%d,\"l\":\"%s\"}",
            (i > 0) ? "," : "",
            i, (unsigned)s->id, (unsigned)s->pressure / 10, (int)s->temperature,
            (s->flags & 0x01) ? 1 : 0, labels[i]);
    }

    // Обнаруженные (сниффер)
    pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"disc\":[");
    for (int i = 0; i < MAX_SENSORS; i++) {
        if (i > 0) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
        if (discovered[i].valid) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "{\"id\":\"%08X\",\"p\":%u,\"t\":%d,\"rssi\":%d}",
                (unsigned)discovered[i].id, (unsigned)discovered[i].pressure,
                (int)discovered[i].temperature, (int)discovered[i].rssi);
        } else {
            pos += snprintf(buf + pos, sizeof(buf) - pos, "null");
        }
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}}");

    bleSendChunked(buf);
}

// =========================================================================
// BLE RX callback
// =========================================================================
static volatile bool cmdReady = false;

static void bleRxCallback(uint16_t conn_hdl) {
    (void)conn_hdl;

    while (bleuart.available()) {
        int c = bleuart.read();
        if (c < 0) break;

        if (c == '\n' || c == '\r') {
            if (cmdBufLen > 0) {
                cmdBuf[cmdBufLen] = 0;
                cmdReady = true;
            }
        } else if (!cmdReady && cmdBufLen < CMD_BUFFER_SIZE - 1) {
            cmdBuf[cmdBufLen++] = (char)c;
        }
    }
}

// =========================================================================
// Обработка JSON команд
// =========================================================================

// Простой парсер JSON: ищет "cmd":"value"
static bool jsonGetString(const char *json, const char *key, char *out, uint8_t outLen) {
    char search[32];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char *p = strstr(json, search);
    if (!p) return false;
    p += strlen(search);
    uint8_t i = 0;
    while (*p && *p != '"' && i < outLen - 1) {
        out[i++] = *p++;
    }
    out[i] = 0;
    return i > 0;
}

static bool jsonGetInt(const char *json, const char *key, int32_t *out) {
    char search[32];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return false;
    p += strlen(search);
    while (*p == ' ') p++;
    if (*p == '"') p++;  // пропускаем кавычки для строковых чисел
    *out = (int32_t)atol(p);
    return true;
}

static bool jsonGetBool(const char *json, const char *key, bool *out) {
    char search[32];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return false;
    p += strlen(search);
    while (*p == ' ') p++;
    if (strncmp(p, "true", 4) == 0) { *out = true; return true; }
    if (strncmp(p, "false", 5) == 0) { *out = false; return true; }
    // Попробуем как число
    *out = (atoi(p) != 0);
    return true;
}

static void processCommand(const char *cmd) {
    char cmdName[32] = {0};
    jsonGetString(cmd, "cmd", cmdName, sizeof(cmdName));

    if (strcmp(cmdName, "status") == 0) {
        sendStatus();
    }
    else if (strcmp(cmdName, "burst") == 0) {
        sendBurst();
    }
    else if (strcmp(cmdName, "stop") == 0) {
        if (snifferActive) sniffer_stop();
        sendLog("Stopped");
    }
    else if (strcmp(cmdName, "reset") == 0) {
        setDefaultConfig();
        configDirty = true;
        radio.setFreq(cfg.freq == 433 ? 433000000UL : 315000000UL);
        sendLog("Config reset to defaults");
    }
    else if (strcmp(cmdName, "sensor") == 0) {
        int32_t sensorIdx = 0;
        jsonGetInt(cmd, "sensor", &sensorIdx);
        if (sensorIdx < 0 || sensorIdx >= MAX_SENSORS) {
            sendLog("Invalid sensor index");
            return;
        }

        char idStr[16] = {0};
        if (jsonGetString(cmd, "id", idStr, sizeof(idStr))) {
            cfg.sensors[sensorIdx].id = (uint32_t)strtoul(idStr, NULL, 16);
        }

        int32_t pressure = 0;
        if (jsonGetInt(cmd, "pressure", &pressure)) {
            cfg.sensors[sensorIdx].pressure = (uint16_t)(pressure * 10);
        }

        int32_t temp = 0;
        if (jsonGetInt(cmd, "temp", &temp)) {
            cfg.sensors[sensorIdx].temperature = (int8_t)temp;
        }

        bool enabled = false;
        if (jsonGetBool(cmd, "enabled", &enabled)) {
            cfg.sensors[sensorIdx].flags = (cfg.sensors[sensorIdx].flags & 0xFE) | (enabled ? 1 : 0);
        }

        configDirty = true;
        char logBuf[64];
        snprintf(logBuf, sizeof(logBuf), "Sensor %d updated", (int)sensorIdx);
        sendLog(logBuf);
    }
    else if (strcmp(cmdName, "autotx") == 0) {
        cfg.tx_enabled = 1;  // всегда включён
        int32_t interval = 0;
        if (jsonGetInt(cmd, "interval", &interval)) {
            cfg.tx_interval = (uint16_t)constrain(interval, 1, 900);
        }
        int32_t packets = 0;
        if (jsonGetInt(cmd, "packets", &packets)) {
            cfg.tx_packets = (uint8_t)constrain(packets, 1, 20);
        }
        lastAutoTx = millis() / 1000 - cfg.tx_interval;  // send immediately
        configDirty = true;
        sendLog("Auto-TX config updated");
    }
    else if (strcmp(cmdName, "settings") == 0) {
        int32_t freq = 0;
        if (jsonGetInt(cmd, "freq", &freq)) {
            cfg.freq = (uint8_t)freq;
            uint32_t freqHz = (freq == 433) ? 433000000UL : 315000000UL;
            radio.setFreq(freqHz);
        }
        int32_t drate = 0;
        if (jsonGetInt(cmd, "drate", &drate)) {
            cfg.datarate = (uint32_t)drate;
            radio.setDRate(cfg.datarate);
        }
        int32_t dev = 0;
        if (jsonGetInt(cmd, "dev", &dev)) {
            cfg.deviation = (uint16_t)dev;
            radio.setDeviation((float)cfg.deviation / 10.0f);
        }
        int32_t pwr = 0;
        if (jsonGetInt(cmd, "pwr", &pwr)) {
            cfg.power = (uint8_t)constrain(pwr, 0, 7);
            radio.setPA(cfg.power);
        }
        configDirty = true;
        sendLog("Settings applied");
    }
    else if (strcmp(cmdName, "battpin") == 0) {
        int32_t pin = 255;
        if (jsonGetInt(cmd, "pin", &pin)) {
            bool validPin = (pin == 255) || (pin >= A0 && pin <= A7);
            if (validPin) {
                cfg.batt_pin = (uint8_t)pin;
                configDirty = true;
                char lb[64];
                if (pin == 255) {
                    snprintf(lb, sizeof(lb), "Batt src: VDDH internal, raw=%d (%d mV)", readBatteryRaw(), readBatteryMv());
                } else {
                    snprintf(lb, sizeof(lb), "Batt pin set to A%d (pin %d), raw=%d", (int)(pin - A0), (int)pin, readBatteryRaw());
                }
                sendLog(lb);
            } else {
                sendLog("Batt pin: 255=VDDH or A0..A7 (Arduino pin 14..21)");
            }
        }
    }
    else if (strcmp(cmdName, "battcal") == 0) {
        int32_t mv = 0;
        if (jsonGetInt(cmd, "mv", &mv) && mv > 0 && mv < 20000) {
            int raw = readBatteryRaw();
            cfg.batt_cal_raw = (uint16_t)raw;
            cfg.batt_cal_mv = (uint16_t)mv;
            configDirty = true;
            char lb[64];
            snprintf(lb, sizeof(lb), "Battery calibrated: raw=%d -> %d mV", raw, (int)mv);
            sendLog(lb);
        } else {
            sendLog("battcal: нужно указать mv (реальное напряжение, мВ)");
        }
    }
    else if (strcmp(cmdName, "sniff_start") == 0) {
        if (cfg.tx_enabled) {
    cfg.tx_enabled = 1;
        }
        sniffer_start();
    }
    else if (strcmp(cmdName, "sniff_stop") == 0) {
        sniffer_stop();
    }
    else if (strcmp(cmdName, "sniff_apply") == 0) {
        int applied = 0;
        for (int i = 0; i < MAX_SENSORS; i++) {
            if (discovered[i].valid) {
                cfg.sensors[i].id = discovered[i].id;
                cfg.sensors[i].pressure = discovered[i].pressure;
                cfg.sensors[i].temperature = discovered[i].temperature;
                cfg.sensors[i].flags = 0x01;  // enabled
                applied++;
            }
        }
        configDirty = true;
        char logBuf[64];
        snprintf(logBuf, sizeof(logBuf), "Applied %d discovered sensors", applied);
        sendLog(logBuf);
    }
    else if (strcmp(cmdName, "license") == 0) {
        char keyStr[16] = {0};
        if (jsonGetString(cmd, "key", keyStr, sizeof(keyStr)) && strlen(keyStr) == 8) {
            // Упрощённая проверка: принимаем любой 8-символьный hex ключ
            uint32_t keyVal = (uint32_t)strtoul(keyStr, NULL, 16);
            cfg.license_key[0] = (keyVal >> 24) & 0xFF;
            cfg.license_key[1] = (keyVal >> 16) & 0xFF;
            cfg.license_key[2] = (keyVal >> 8) & 0xFF;
            cfg.license_key[3] = keyVal & 0xFF;
            licenseValid = 2;
            configDirty = true;
            sendLog("License activated");
        } else {
            sendLog("Invalid license key");
        }
    }
    else if (cmdName[0] == 't' && cmdName[1] == 'x' && cmdName[2] >= '1' && cmdName[2] <= '4') {
        // tx1, tx2, tx3, tx4 - отправка одного датчика
        uint8_t idx = cmdName[2] - '1';
        if (snifferActive) sniffer_stop();
        send_pmv107j_sensor(idx);
    }
    else {
        char logBuf[64];
        snprintf(logBuf, sizeof(logBuf), "Unknown command: %s", cmdName);
        sendLog(logBuf);
    }
}

// =========================================================================
// Настройка BLE
// =========================================================================
static void setupBLE() {
    Bluefruit.configPrphConn(247, 16, 4, 4);  // MTU, event_len, hvn_qsize, wrcmd_qsize
    Bluefruit.begin();
    Bluefruit.setTxPower(0);    // 0 dBm — reliable connection
    Bluefruit.setName("TPMS-NRF52840");

    // Connection callbacks
    Bluefruit.Periph.setConnectCallback([](uint16_t conn_handle) {
        bleConnected = true;
        bleHadConnection = true;  // было реальное подключение → можно спать при отключении
        // Don't send any connection here — phone hasn't written CCCD yet
    });
    Bluefruit.Periph.setDisconnectCallback([](uint16_t conn_handle, uint8_t reason) {
        bleConnected = false;
        if (snifferActive) sniffer_stop();
        // Авто-сон отключён: устройство остаётся в анонсе, чтобы телефон мог переподключиться.
        // (Сон можно добавить позже, когда будет готова стабильная работа с батареей.)
    });

    // Configure and Start Device Information Service
    bledis.setManufacturer("TPMS-Emulator");
    bledis.setModel("nRF52840+CC1101");
    bledis.begin();

    // Configure and Start BLE Uart Service
    bleuart.begin();

    // Set up advertising
    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
    Bluefruit.Advertising.addTxPower();
    Bluefruit.Advertising.addService(bleuart);

    // Secondary Scan Response packet (optional)
    Bluefruit.ScanResponse.addName();

    Bluefruit.Advertising.restartOnDisconnect(true);
    Bluefruit.Advertising.setInterval(160, 160);  // in unit of 0.625 ms
    Bluefruit.Advertising.setFastTimeout(30);
    Bluefruit.Advertising.start(0);               // 0 = Don't stop advertising after n seconds
}

// =========================================================================
// Индикация светодиодом
// =========================================================================
static void blinkLed(uint8_t times, uint16_t delayMs) {
    pinMode(PIN_STATUS_LED, OUTPUT);
    for (uint8_t i = 0; i < times; i++) {
        digitalWrite(PIN_STATUS_LED, HIGH);
        delay(delayMs);
        digitalWrite(PIN_STATUS_LED, LOW);
        delay(delayMs);
    }
}

// =========================================================================
// SETUP
// =========================================================================
void setup() {
    // Инициализация Serial (для отладки)
    Serial.begin(115200);
    Serial.println("DBG: setup start");
    Serial.flush();

    // LED
    pinMode(PIN_STATUS_LED, OUTPUT);
    digitalWrite(PIN_STATUS_LED, LOW);

    // Wake button (active LOW with pull-up)
    pinMode(PIN_WAKE_BTN, INPUT_PULLUP);

    // CC1101 питание
    cc1101PowerOn();

    // Загрузка конфигурации
    loadConfig();
    Serial.println("DBG: config loaded");
    Serial.flush();
    cfg.tx_enabled = 1;  // авто-TX всегда включён после старта
    lastAutoTx = millis() / 1000 - cfg.tx_interval;  // отправить сразу при старте

    // Инициализация BLE (до CC1101, так как Bluetooth тоже использует радиочастоту)
    Serial.println("DBG: before setupBLE");
    Serial.flush();
    setupBLE();
    Serial.println("DBG: after setupBLE");
    Serial.flush();

    // Инициализация CC1101
    Serial.println("DBG: before radio.init");
    Serial.flush();
    radio.init();
    Serial.println("DBG: after radio.init");
    Serial.flush();
    sendLog("CC1101 initialized OK");
    // Настраиваем частоту из конфига
    uint32_t freqHz = (cfg.freq == 433) ? 433000000UL : 315000000UL;
    radio.setFreq(freqHz);
    radio.setPA(cfg.power);
    // Частота и девиация из конфига (как было раньше — radio.init() дефолты)
    radio.setFreq(freqHz);
    radio.setPA(cfg.power);
    radio.setDRate(cfg.datarate);
    radio.setDeviation((float)cfg.deviation / 10.0f);

    // BLE RX callback
    bleuart.setRxCallback(bleRxCallback);

    // Стартовая индикация
    blinkLed(3, 200);
    sendLog("TPMS-NRF52840 ready");
    sendStatus();
    Serial.println("DBG: setup done");
    Serial.flush();

    // BLE power management timer
    bleStartTime = millis();
}

// =========================================================================
// MAIN LOOP
// =========================================================================
void loop() {
    // 0. BLE power management отключён: устройство анонсируется непрерывно, пока работает.
    //    (Сон отключён на время отладки, чтобы телефон всегда мог найти и переподключиться.)

    // 1. Обработка BLE команд
    if (cmdReady) {
        processCommand(cmdBuf);
        cmdBufLen = 0;
        cmdReady = false;
    }

    // 1.1 Отложенное сохранение конфига (не в BLE callback!)
    if (configDirty) {
        configDirty = false;
        saveConfig();
    }

    // 2. Сниффер
    if (snifferActive) {
        sniffer_loop();
    }

    // 3. Авто-TX
    if (cfg.tx_enabled && !snifferActive) {
        uint32_t now = millis() / 1000;
        if (now - lastAutoTx >= cfg.tx_interval) {
            lastAutoTx = now;
            for (uint8_t i = 0; i < cfg.tx_packets; i++) {
                for (uint8_t s = 0; s < MAX_SENSORS; s++) {
                    if (cfg.sensors[s].flags & 0x01) {
                        send_pmv107j_sensor(s);
                        delay(30);
                    }
                }
                if (i < cfg.tx_packets - 1) delay(100);
            }
        }
    }

    // 4. Проверка батареи (только индикация, НЕ отключает авто-TX).
    //    Ранее при "критичной" батарее авто-TX блокировался, но датчик батареи на A4
    //    может читать ~0 мВ (делитель/пин не подключён), что отключало передачу. Убираем блокировку.
    if (isBatteryCritical()) {
        static uint32_t lastBattWarn = 0;
        if (millis() - lastBattWarn > 30000) {
            lastBattWarn = millis();
            sendLog("Battery low warning");
        }
    }

    // 5. LED индикация при передаче
    static uint32_t lastBlink = 0;
    if (cfg.tx_enabled && (millis() - lastBlink > 1000)) {
        lastBlink = millis();
        digitalWrite(PIN_STATUS_LED, !digitalRead(PIN_STATUS_LED));
    }

    delay(1);
}
