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
#include <Adafruit_LittleFS.h>
#include <InternalFS.h>

#include "CC1101.h"

using namespace Adafruit_LittleFS_Namespace;

// =========================================================================
// Pin Mapping - ProMicro nRF52840 V1940
// ВСЕ пины должны быть согласованы во ВСЕХ файлах проекта!
// =========================================================================
#define PIN_SPI_MISO    29    // Arduino pin 29 = P0.17 (board label "017")
#define PIN_SPI_MOSI    20    // Arduino pin 20 = P0.29 (board label "029")
#define PIN_SPI_SCK     21    // Arduino pin 21 = P0.31 (board label "031")
#define PIN_CC1101_CS   2     // Arduino pin 2  = P0.10 (board label "010")
#define PIN_CC1101_GDO0 11    // Arduino pin 11 = P0.06 (board label "006")
#define PIN_CC1101_GDO2 12    // Arduino pin 12 = P0.08 (board label "008")
#define PIN_CC1101_PWR  28    // Arduino pin 28 = P0.20 (board label "020")
#define PIN_STATUS_LED  24    // Arduino pin 24 = P0.15 (onboard LED)
#define PIN_BATTERY_ADC 18    // Arduino pin 18 = P0.02 (board label "002"), AIN4

// =========================================================================
// Конфигурация по умолчанию
// =========================================================================
#define CONFIG_MAGIC         0xB0
#define CONFIG_FILENAME      "tpms_cfg"
#define MAX_SENSORS          4
#define MAX_PAYLOAD_BITS     72     // 66 бит payload + preamble
#define DEFAULT_FREQ_315     315
#define DEFAULT_FREQ_433     433
#define DEFAULT_DATARATE     10000  // 10 kbaud
#define DEFAULT_DEVIATION    38000  // 38 kHz
#define DEFAULT_POWER        0x1E   // ~10 dBm
#define DEFAULT_INTERVAL     300    // секунд между burst
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
static File file(InternalFS);

// BLE
static BLEDis  bledis;
static BLEUart bleuart;

// Буфер команд
static char cmdBuf[CMD_BUFFER_SIZE];
static uint16_t cmdBufLen = 0;

// Состояние
static bool snifferActive = false;
static uint32_t lastAutoTx = 0;
static uint32_t trialStartTime = 0;
static uint32_t licenseValid = 0; // 0 = no license, 1 = trial, 2 = full

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
static uint16_t readBatteryMv() {
    analogReadResolution(12);
    int adc = analogRead(A4);  // PIN_BATTERY_ADC = 18 = A4
    // Делитель 1:2, Vref = 3.3V, 12-bit ADC
    // Vbat = adc * 2 * 3300 / 4096
    uint32_t mv = (uint32_t)adc * 6600UL / 4096UL;
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

// =========================================================================
// BLE helpers - chunked write для избежания FIFO overflow
// =========================================================================
static void bleSendChunked(const char *str) {
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

// =========================================================================
// CRC-8 для PMV-107J: полином 0x13, init 0x00
// =========================================================================
static uint8_t calculate_crc8_pmv(const uint8_t *bits, uint16_t bitLen) {
    uint8_t crc = 0x00;
    // Обрабатываем первые 58 бит (до поля CRC)
    uint16_t crcBits = (bitLen > 58) ? 58 : bitLen;

    for (uint16_t i = 0; i < crcBits; i++) {
        uint8_t bit = get_bit(bits, i);
        uint8_t fb = crc ^ bit;
        crc >>= 1;
        if (fb & 1) crc ^= 0x8C;  // реверс полинома 0x13 -> 0x8C
    }
    return crc;
}

// =========================================================================
// PMV-107J: построение 66-битного payload
// Структура payload (66 бит):
//   [0:27]   - ID (28 бит, LSB first)
//   [28]     - флаг батареи (0=OK, 1=low)
//   [29:30]  - счётчик (2 бита)
//   [31:32]  - флаги (2 бита, обычно 0b11)
//   [33:43]  - давление (11 бит, инвертированное)
//   [44:54]  - давление (11 бит, прямое) = инверсия предыдущего
//   [55:57]  - температура (3 бита, XOR 0b111)
//   [58:65]  - CRC-8 (8 бит)
// =========================================================================
static uint16_t build_pmv107j_payload(uint8_t *outBits, uint32_t sensorId,
                                       uint16_t pressure, int8_t temperature,
                                       uint8_t counter, uint8_t batFlag) {
    // Очищаем массив
    uint16_t totalBits = 16 + 6 + 66; // preamble(16) + sync(6) + payload(66) = 88 бит
    // Но мы строим только payload (66 бит), preamble+sync добавляются отдельно
    uint16_t payloadBits = 66;
    uint16_t arrLen = (payloadBits + 7) / 8;
    memset(outBits, 0, arrLen);

    // Давление: PMV-107J использует кодировку давления
    // Давление в кПа * 10 (2300 = 230.0 кПа ≈ 33.4 psi)
    // Кодирование: val = 1800 - pressure_kpa10 (инвертированное)
    // Прямое: pressure_kpa10
    uint16_t presEncoded = pressure;
    if (presEncoded < 500) presEncoded = 500;
    if (presEncoded > 3500) presEncoded = 3500;

    // Инвертированное давление
    uint16_t presInv = 4095 - presEncoded;

    // Температура: кодирование 3 бит
    // -40°C=0, -30°C=1, ... +30°C=7, +40°C=7
    int8_t tempEnc = (temperature + 40) / 10;
    if (tempEnc < 0) tempEnc = 0;
    if (tempEnc > 7) tempEnc = 7;
    uint8_t tempXor = tempEnc ^ 0x07;

    // ID: 28 бит, LSB first
    for (int i = 0; i < 28; i++) {
        set_bit(outBits, i, (sensorId >> i) & 1);
    }

    // Флаг батареи (bit 28)
    set_bit(outBits, 28, batFlag);

    // Счётчик (bits 29-30)
    set_bit(outBits, 29, (counter >> 0) & 1);
    set_bit(outBits, 30, (counter >> 1) & 1);

    // Флаги (bits 31-32), обычно 0b11
    set_bit(outBits, 31, 1);
    set_bit(outBits, 32, 1);

    // Давление инвертированное (bits 33-43), 11 бит, LSB first
    for (int i = 0; i < 11; i++) {
        set_bit(outBits, 33 + i, (presInv >> i) & 1);
    }

    // Давление прямое (bits 44-54), 11 бит, LSB first
    for (int i = 0; i < 11; i++) {
        set_bit(outBits, 44 + i, (presEncoded >> i) & 1);
    }

    // Температура XOR (bits 55-57), 3 бита
    set_bit(outBits, 55, (tempXor >> 0) & 1);
    set_bit(outBits, 56, (tempXor >> 1) & 1);
    set_bit(outBits, 57, (tempXor >> 2) & 1);

    // CRC-8 (bits 58-65)
    uint8_t crc = calculate_crc8_pmv(outBits, 58);
    for (int i = 0; i < 8; i++) {
        set_bit(outBits, 58 + i, (crc >> i) & 1);
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
    // DM encoding удваивает длину: каждый бит → 2 бита
    uint16_t outLen = inLen * 2;
    uint16_t arrLen = (outLen + 7) / 8;
    memset(outBits, 0, arrLen);

    // Начальное состояние: предыдущий символ = 1
    uint8_t prevLevel = 1;
    uint16_t outIdx = 0;

    for (uint16_t i = 0; i < inLen; i++) {
        uint8_t bit = get_bit(inBits, i);

        if (bit == 0) {
            // Бит 0: переход в середине
            set_bit(outBits, outIdx++, prevLevel);
            prevLevel = prevLevel ? 0 : 1;  // переход
            set_bit(outBits, outIdx++, prevLevel);
        } else {
            // Бит 1: без перехода
            set_bit(outBits, outIdx++, prevLevel);
            set_bit(outBits, outIdx++, prevLevel);
        }
    }

    return outIdx;
}

// =========================================================================
// Построение полного пакета PMV-107J
// Preamble: 16 нулей + "111110" (6 бит) + 66 бит payload
// Всего: 88 бит → DM encode → 176 бит → 22 байта
// =========================================================================
static uint16_t build_pmv107j_packet(uint8_t *packet, uint32_t sensorId,
                                      uint16_t pressure, int8_t temperature,
                                      uint8_t counter, uint8_t batFlag) {
    // Шаг 1: Строим preamble + sync + payload = 88 бит
    uint8_t rawBits[24]; // 88 бит = 11 байт
    memset(rawBits, 0, sizeof(rawBits));

    // Preamble: 16 нулей (уже нули от memset)
    // Sync: "111110" в позициях 16-21
    uint8_t syncPat[] = {1, 1, 1, 1, 1, 0};
    for (int i = 0; i < 6; i++) {
        set_bit(rawBits, 16 + i, syncPat[i]);
    }

    // Payload: 66 бит начиная с позиции 22
    uint16_t payloadBits = build_pmv107j_payload(rawBits + 2,  // смещение на 16 бит = 2 байта
                                                    sensorId, pressure, temperature,
                                                    counter, batFlag);
    // payloadBits = 66, позиция в rawBits = 22
    // build_pmv107j_payload пишет в outBits начиная с бита 0
    // Нам нужно сдвинуть на 22 бита
    // Переписываем: сначала строим payload в отдельный массив
    uint8_t payloadArr[16];
    memset(payloadArr, 0, sizeof(payloadArr));
    payloadBits = build_pmv107j_payload(payloadArr, sensorId, pressure,
                                         temperature, counter, batFlag);

    // Копируем payload в rawBits начиная с бита 22
    for (uint16_t i = 0; i < payloadBits; i++) {
        set_bit(rawBits, 22 + i, get_bit(payloadArr, i));
    }

    uint16_t totalBits = 22 + payloadBits; // 88 бит

    // Шаг 2: Differential Manchester Encoding
    uint8_t dmBits[32]; // 176 бит = 22 байта
    uint16_t dmLen = differential_manchester_encode(rawBits, totalBits, dmBits);

    // Шаг 3: Конвертируем биты в байты
    uint16_t byteLen = (dmLen + 7) / 8;
    memset(packet, 0, byteLen + 1);
    for (uint16_t i = 0; i < dmLen; i++) {
        if (get_bit(dmBits, i)) {
            packet[i / 8] |= (1 << (7 - (i % 8)));
        }
    }

    return byteLen;
}

// =========================================================================
// Отправка сырых данных через CC1101 (fixed-length TX)
// =========================================================================
static void cc1101_send_raw(const uint8_t *data, uint8_t len) {
    radio.setIdleState();
    radio.flushTxFifo();

    // Устанавливаем длину пакета
    radio.writeReg(CC1101_PKTLEN, len);

    // Пишем данные в FIFO
    radio.writeBurstReg(CC1101_TXFIFO, data, len);

    // Старт TX
    radio.sendCommand(CC1101_STX);

    // Ждём окончания передачи
    unsigned long start = millis();
    while (millis() - start < 100) {
        uint8_t state = radio.getMarcState();
        if (state == CC1101_MARCSTATE_IDLE || state == 0x01) break;
        delayMicroseconds(100);
    }

    radio.setIdleState();
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
             "{\"t\":\"pkt\",\"s\":%d,\"id\":\"%06X\",\"p\":%d,\"tmp\":%d,\"c\":%d,\"crc\":\"OK\"}",
             sensorIdx, (unsigned)s->id, (unsigned)s->pressure, (int)s->temperature, counter);
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
            delay(50);  // пауза между датчиками
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
    crcCalc = calculate_crc8_pmv(payload58, 58);

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
    radio.setIdleState();
    radio.flushRxFifo();

    // Настраиваем для приёма
    radio.writeReg(CC1101_PKTCTRL0, 0x32);  // Fixed length, no CRC
    radio.writeReg(CC1101_PKTLEN, 0xFF);     // максимальная длина
    radio.writeReg(CC1101_MDMCFG2, 0x10);   // 2-FSK
    radio.writeReg(CC1101_IOCFG0, 0x06);    // GDO0 = sync detect

    radio.setRxState();
    snifferActive = true;

    for (int i = 0; i < MAX_SENSORS; i++) {
        discovered[i].valid = false;
    }

    sendLog("Sniffer started");
}

static void sniffer_stop() {
    radio.setIdleState();
    snifferActive = false;
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
                 "{\"t\":\"pkt\",\"id\":\"%06X\",\"p\":%d,\"tmp\":%d,\"c\":%d,\"rssi\":%d}",
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
    cfg.tx_enabled = 0;
    cfg.tx_interval = DEFAULT_INTERVAL;
    cfg.tx_packets = DEFAULT_PACKETS;
    cfg.freq = DEFAULT_FREQ_315;
    cfg.batt_mah = 500;

    // Датчики по умолчанию (Acura RDX 2008)
    cfg.sensors[0].id = 0x00298088; cfg.sensors[0].pressure = 2300; cfg.sensors[0].temperature = 20; cfg.sensors[0].flags = 0x01;
    cfg.sensors[1].id = 0x0466E088; cfg.sensors[1].pressure = 2300; cfg.sensors[1].temperature = 20; cfg.sensors[1].flags = 0x01;
    cfg.sensors[2].id = 0x0D784088; cfg.sensors[2].pressure = 2300; cfg.sensors[2].temperature = 20; cfg.sensors[2].flags = 0x01;
    cfg.sensors[3].id = 0x0C765088; cfg.sensors[3].pressure = 2300; cfg.sensors[3].temperature = 20; cfg.sensors[3].flags = 0x01;

    trialStartTime = millis() / 1000;
}

static bool loadConfig() {
    InternalFS.begin();

    if (!file.open(CONFIG_FILENAME, FILE_O_READ)) {
        sendLog("Config not found, using defaults");
        setDefaultConfig();
        saveConfig();
        return false;
    }

    TPMSConfig tmp;
    size_t readLen = file.read(&tmp, sizeof(tmp));
    file.close();

    if (readLen != sizeof(tmp) || tmp.magic != CONFIG_MAGIC) {
        sendLog("Config corrupt, using defaults");
        setDefaultConfig();
        saveConfig();
        return false;
    }

    memcpy(&cfg, &tmp, sizeof(cfg));
    trialStartTime = cfg.trial_start;

    // Проверяем лицензию
    if (cfg.license_key[0] || cfg.license_key[1] || cfg.license_key[2] || cfg.license_key[3]) {
        licenseValid = 2;  // полная лицензия
    } else if (trialStartTime > 0) {
        uint32_t elapsed = (millis() / 1000) - trialStartTime;
        licenseValid = (elapsed < TRIAL_SECONDS) ? 1 : 0;
    }

    sendLog("Config loaded");
    return true;
}

static bool saveConfig() {
    InternalFS.begin();

    cfg.trial_start = trialStartTime;

    file.open(CONFIG_FILENAME, FILE_O_WRITE);
    size_t written = file.write(&cfg, sizeof(cfg));
    file.close();

    if (written != sizeof(cfg)) {
        sendLog("Config save FAILED");
        return false;
    }
    InternalFS.end();
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
        "\"batt_mv\":%u,\"batt_pct\":%u,\"license\":%u,"
        "\"sniff\":%d,",
        cfg.freq, (unsigned long)cfg.datarate, (unsigned)cfg.deviation, (unsigned)cfg.power,
        cfg.tx_enabled, (unsigned)cfg.tx_interval, (unsigned)cfg.tx_packets,
        (unsigned)readBatteryMv(), (unsigned)getBatteryPercent(),
        (unsigned)licenseValid, snifferActive ? 1 : 0);

    // Датчики
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\"sensors\":[");
    const char *labels[] = {"PL", "PP", "ZL", "ZP"};
    for (int i = 0; i < MAX_SENSORS; i++) {
        SensorConfig *s = &cfg.sensors[i];
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"i\":%d,\"id\":\"%06X\",\"p\":%u,\"t\":%d,\"en\":%d,\"l\":\"%s\"}",
            (i > 0) ? "," : "",
            i, (unsigned)s->id, (unsigned)s->pressure, (int)s->temperature,
            (s->flags & 0x01) ? 1 : 0, labels[i]);
    }

    // Обнаруженные (сниффер)
    pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"disc\":[");
    for (int i = 0; i < MAX_SENSORS; i++) {
        if (i > 0) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
        if (discovered[i].valid) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "{\"id\":\"%06X\",\"p\":%u,\"t\":%d,\"rssi\":%d}",
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
static void bleRxCallback(uint16_t conn_hdl) {
    (void)conn_hdl;

    while (bleuart.available()) {
        int c = bleuart.read();
        if (c < 0) break;

        if (c == '\n' || c == '\r') {
            if (cmdBufLen > 0) {
                cmdBuf[cmdBufLen] = 0;
                cmdBufLen = 0;
                // Команда будет обработана в main loop
            }
        } else if (cmdBufLen < CMD_BUFFER_SIZE - 1) {
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
    else if (strcmp(cmdName, "tx") == 0) {
        cfg.tx_enabled = 1;
        lastAutoTx = millis() / 1000 - cfg.tx_interval;  // отправить сразу
        sendLog("Auto-TX started");
    }
    else if (strcmp(cmdName, "stop") == 0) {
        cfg.tx_enabled = 0;
        if (snifferActive) sniffer_stop();
        radio.setIdleState();
        sendLog("Stopped");
    }
    else if (strcmp(cmdName, "reset") == 0) {
        setDefaultConfig();
        saveConfig();
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
            cfg.sensors[sensorIdx].pressure = (uint16_t)pressure;
        }

        int32_t temp = 0;
        if (jsonGetInt(cmd, "temp", &temp)) {
            cfg.sensors[sensorIdx].temperature = (int8_t)temp;
        }

        bool enabled = false;
        if (jsonGetBool(cmd, "enabled", &enabled)) {
            cfg.sensors[sensorIdx].flags = (cfg.sensors[sensorIdx].flags & 0xFE) | (enabled ? 1 : 0);
        }

        saveConfig();
        char logBuf[64];
        snprintf(logBuf, sizeof(logBuf), "Sensor %d updated", (int)sensorIdx);
        sendLog(logBuf);
    }
    else if (strcmp(cmdName, "autotx") == 0) {
        bool enabled = false;
        if (jsonGetBool(cmd, "enabled", &enabled)) {
            cfg.tx_enabled = enabled ? 1 : 0;
        }
        int32_t interval = 0;
        if (jsonGetInt(cmd, "interval", &interval)) {
            cfg.tx_interval = (uint16_t)constrain(interval, 5, 900);
        }
        int32_t packets = 0;
        if (jsonGetInt(cmd, "packets", &packets)) {
            cfg.tx_packets = (uint8_t)constrain(packets, 1, 20);
        }
        saveConfig();
        sendLog("Auto-TX config updated");
    }
    else if (strcmp(cmdName, "settings") == 0) {
        int32_t freq = 0;
        if (jsonGetInt(cmd, "freq", &freq)) {
            cfg.freq = (uint8_t)freq;
            uint32_t freqHz = (freq == 433) ? 433000000UL : 315000000UL;
            radio.setFreq(freqHz);
            sendLog("Frequency changed");
        }
        saveConfig();
    }
    else if (strcmp(cmdName, "sniff_start") == 0) {
        if (cfg.tx_enabled) {
            cfg.tx_enabled = 0;
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
        saveConfig();
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
            saveConfig();
            sendLog("License activated");
        } else {
            sendLog("Invalid license key");
        }
    }
    else if (cmdName[0] == 't' && cmdName[1] == 'x' && cmdName[2] >= '1' && cmdName[2] <= '4') {
        // tx1, tx2, tx3, tx4 - отправка одного датчика
        uint8_t idx = cmdName[2] - '1';
        if (cfg.tx_enabled) cfg.tx_enabled = 0;  // останавливаем авто-TX
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
    Bluefruit.begin();
    Bluefruit.setTxPower(4);    // Check valid values for the board
    Bluefruit.setName("TPMS-NRF52840");

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

    // LED
    pinMode(PIN_STATUS_LED, OUTPUT);
    digitalWrite(PIN_STATUS_LED, LOW);

    // CC1101 питание
    cc1101PowerOn();

    // Загрузка конфигурации
    InternalFS.begin();
    loadConfig();

    // Инициализация BLE (до CC1101, так как Bluetooth тоже использует радиочастоту)
    setupBLE();

    // Инициализация CC1101
    if (!radio.init()) {
        sendLog("CC1101 init FAILED!");
        blinkLed(10, 100);
    } else {
        sendLog("CC1101 initialized OK");
        // Настраиваем частоту из конфига
        uint32_t freqHz = (cfg.freq == 433) ? 433000000UL : 315000000UL;
        radio.setFreq(freqHz);
        radio.setPA(cfg.power);

        // Режим: IDLE (ждём команд через BLE)
        radio.setIdleState();
    }

    // BLE RX callback
    bleuart.setRxCallback(bleRxCallback);

    // Стартовая индикация
    blinkLed(3, 200);
    sendLog("TPMS-NRF52840 ready");
    sendStatus();
}

// =========================================================================
// MAIN LOOP
// =========================================================================
void loop() {
    // 1. Обработка BLE команд
    if (cmdBufLen > 0 && (cmdBuf[cmdBufLen - 1] == '}' || cmdBuf[cmdBufLen - 1] == '\n')) {
        cmdBuf[cmdBufLen] = 0;
        processCommand(cmdBuf);
        cmdBufLen = 0;
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

    // 4. Проверка батареи
    if (isBatteryCritical() && cfg.tx_enabled) {
        cfg.tx_enabled = 0;
        if (snifferActive) sniffer_stop();
        radio.setIdleState();
        sendLog("Battery critical! Stopped.");
        blinkLed(5, 500);

        // Спим для экономии
        delay(5000);
    }

    // 5. LED индикация при передаче
    static uint32_t lastBlink = 0;
    if (cfg.tx_enabled && (millis() - lastBlink > 1000)) {
        lastBlink = millis();
        digitalWrite(PIN_STATUS_LED, !digitalRead(PIN_STATUS_LED));
    }

    delay(1);
}
