# TPMS NRF52840 — Эмулятор/Сниффер на nRF52840

Чистый nRF5 SDK (без Arduino) + BLE 5.0.

## Платы

| Плата | Зарядка LiPo | Цена |
|-------|-------------|------|
| Seeed XIAO nRF52840 | Встроенная | ~$6 |
| Adafruit Feather nRF52840 | Встроенная | ~$20 |
| Pro Mini nRF52840 | Нет (нужен TP4056) | ~$5 |

## Подключение CC1101

```
nRF52840    →  CC1101
─────────────────────
P0.13       →  SI/DIN    SPI MOSI
P0.15       →  SO/DOUT   SPI MISO
P0.14       →  CLK       SPI Clock
P0.16       →  CSN       Chip Select
P0.11       →  GDO0      TX/RX done
P0.12       →  GDO2      Optional
P0.10       →  VCC       Power (HIGH=ON)
GND         →  GND       Ground
```

## Сборка

```bash
# Скачайте nRF5 SDK 17.1.0 и укажите путь:
export SDK_ROOT=/path/to/nRF5_SDK_17.1.0

# Сборка
make

# Прошивка
make flash
```

## BLE

Устройство рекламируется как **TPMS-NRF**.

Сервис UUID: `12345678-1234-5678-1234-56789abcdef0`

| Характеристика | UUID | Тип |
|---------------|------|-----|
| Sensor Data | 0x1235 | Notify |
| Config | 0x1236 | Read/Write |
| Command | 0x1237 | Write |
| Status | 0x1238 | Read/Notify |

### Команды

| Код | Действие |
|-----|----------|
| 0x01 | Запустить сниффер |
| 0x02 | Остановить сниффер |
| 0x03 | Запустить эмулятор |
| 0x04 | Остановить эмулятор |
| 0x05 | Установить конфигурацию |
| 0x06 | Получить конфигурацию |
| 0x07 | Получить статус |
| 0x08 | Перезагрузка |

## Энергопотребление

| Режим | Ток | Время (3000mAh) |
|-------|-----|-----------------|
| Deep Sleep | ~1µA | ~342 года |
| BLE Advertising | ~10µA | ~34 года |
| BLE + Emulator | ~5mA | ~25 дней |
| BLE + Sniffer | ~15mA | ~8 дней |

## Статус

- [x] CC1101 SPI драйвер (bare metal nRF5 SDK)
- [x] BLE TPMS сервис
- [x] Эмулятор 4 датчиков
- [ ] Сниффер (нужен dm_decode)
- [ ] Deep sleep
- [ ] Контроль батареи (ADC)
- [ ] OTA через BLE

## Лицензия

MIT
