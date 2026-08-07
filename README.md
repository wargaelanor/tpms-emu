# TPMS Emulator

Единый репозиторий для TPMS-эмулятора/сниффера — три платформы в одном месте.

## Структура

| Папка | Платформа | Что это |
|-------|-----------|---------|
| `esp/` | ESP8266 D1 Mini, ESP32-C3 | Эмулятор/сниффер PMV-107J (315/433 МГц), веб-интерфейс, deep sleep |
| `nrf/` | nRF52840 | Эмулятор + BLE, Android-приложение, Web-BLE PWA |

История обоих репозиториев (tags/коммиты) сохранена:
- `esp/` — из TPMS-Emulator (ESP8266/ESP32-C3)
- `nrf/` — из TPMS-NRF52840

## Сборка

генератор ключей https://wargaelanor.github.io/tpms-emu/tools/keygen.html

Обе платформы собираются через [PlatformIO](https://platformio.org):

```bash
# ESP8266 эмулятор
cd esp
pio run -e esp8266-d1mini

# ESP32-C3 сниффер
pio run -e esp32-c3-devkitm-1

# nRF52840
cd nrf
pio run
```
