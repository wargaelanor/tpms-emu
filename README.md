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

## Приложения (онлайн)

| Приложение | Ссылка |
|------------|--------|
| **TPMS Tool** — генератор ключей + прошивальщик ESP (WebSerial) | [wargaelanor.github.io/tpms-emu/tools/keygen.html](https://wargaelanor.github.io/tpms-emu/tools/keygen.html) |
| **Прошивки (последний релиз)** — ESP32-C3, ESP8266, nRF52840 (.uf2), APK | [github.com/wargaelanor/tpms-emu/releases/latest](https://github.com/wargaelanor/tpms-emu/releases/latest) |
| **Web-BLE приложение** для nRF52840 (PWA) | [wargaelanor.github.io/tpms-emu/nrf/webapp/](https://wargaelanor.github.io/tpms-emu/nrf/webapp/) |
| **Android APK** (nRF52840) | [releases/latest/download/tpms-nrf52840-debug.apk](https://github.com/wargaelanor/tpms-emu/releases/latest/download/tpms-nrf52840-debug.apk) |

## Сборка

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
