# TPMS Emulator / Sniffer для ProMicro nRF52840 V1940

Порт проекта [TPMS-Emulator](https://git.evgeniys.keenetic.pro/ai_agent/TPMS-Emulator) на платы **ProMicro nRF52840 V1940** (клон Nice!Nano).

Вместо WiFi/Web-интерфейса используется **Bluetooth LE + Android-приложение**.

## Возможности

- Эмуляция 4 TPMS-датчиков PMV-107J (315 / 433 МГц)
- Сниффер реальных датчиков PMV-107J
- Управление через Android-приложение по Bluetooth
- Сохранение настроек во внутренней Flash
- Индикация состояния светодиодом
- Контроль напряжения батареи (делитель 1:2 на A0)

## Схема подключения CC1101

| Сигнал | Feather D | nRF52840 GPIO | **Метка на плате** |
|--------|-----------|---------------|---------------------|
| MOSI   | D48       | P0.31         | **031**             |
| MISO   | D46       | P0.29         | **029**             |
| SCK    | D3        | P1.04         | **104**             |
| CSN    | D25       | P0.06         | **006**             |
| GDO0   | D27       | P0.08         | **008**             |
| GDO2   | D10       | P1.13         | **113**             |
| POWER  | D8        | P1.11         | **111**             |
| LED    | D12       | P1.15         | **115**             |
| BATT   | 2 (AIN0)  | P0.02         | **002**             |

> **Важно:** CC1101 питается напрямую от GPIO пина `POWER` (D8 / P1.11 / метка **111**). Убедитесь, что модуль потребляет не более допустимого тока GPIO nRF52840, или используйте MOSFET/транзистор для управления питанием.

Подробная инструкция по пайке: [CC1101_WIRING.md](CC1101_WIRING.md)

## Сборка прошивки

Требуется PlatformIO (CLI или VS Code плагин).

```bash
cd android/../..   # корень TPMS-NRF52840
pio run
```

## Прошивка через UF2

1. Дважды нажмите **RESET** на плате — появится USB-диск (обычно `E:` с меткой `NICENANO`).
2. Скопируйте `firmware.uf2` на этот диск.
3. Плата перезагрузится и запустится.

Для конвертации `.hex` в `.uf2`:

```bash
py uf2conv.py -c -f NRF52840 -o firmware.uf2 .pio/build/promicro_nrf52840/firmware.hex
```

## Android-приложение

Приложение находится в папке `android/`. Сборка через Android Studio или Gradle:

```bash
cd android
./gradlew assembleDebug
```

APK будет в `android/app/build/outputs/apk/debug/app-debug.apk`.

### Разрешения

Приложение запрашивает:
- Bluetooth Scan / Connect (Android 12+)
- Location (для сканирования на Android 11 и ниже)

### Использование

1. Включите Bluetooth на телефоне.
2. Откройте приложение, нажмите **Поиск**.
3. Выберите устройство **TPMS-NRF52840**.
4. Нажмите **Подключить**.
5. Приложение автоматически запросит статус и обновит UI.

## BLE-протокол

Используется стандартный сервис **Nordic UART Service (NUS)**:

- Service UUID: `6e400001-b5a3-f393-e0a9-e50e24dcca9e`
- RX (phone → device): `6e400002-b5a3-f393-e0a9-e50e24dcca9e`
- TX (device → phone): `6e400003-b5a3-f393-e0a9-e50e24dcca9e`

Все команды и ответы — JSON, каждое сообщение заканчивается `\n`.

### Команды

| Команда | Пример | Описание |
|---------|--------|----------|
| status | `{"cmd":"status"}` | Получить полный статус |
| burst | `{"cmd":"burst"}` | Отправить burst всем включённым датчикам |
| tx | `{"cmd":"tx"}` | Включить авто-передачу |
| stop | `{"cmd":"stop"}` | Выключить авто-передачу |
| sensor | `{"cmd":"sensor","sensor":0,"id":"02C03839","pressure":230,"temp":20,"enabled":true}` | Сохранить датчик |
| tx1..tx4 | `{"cmd":"tx1"}` | Отправить один датчик |
| autotx | `{"cmd":"autotx","enabled":true,"interval":300,"packets":2}` | Настроить авто-передачу |
| settings | `{"cmd":"settings","freq":315}` | Сменить частоту |
| sniff_start | `{"cmd":"sniff_start"}` | Запустить сниффер |
| sniff_stop | `{"cmd":"sniff_stop"}` | Остановить сниффер |
| sniff_apply | `{"cmd":"sniff_apply"}` | Применить найденные датчики |
| license | `{"cmd":"license","key":"A1B2C3D4"}` | Активировать лицензию |
| reset | `{"cmd":"reset"}` | Сбросить настройки |

### Ответы

```json
{"t":"status","data":{...}}
{"t":"log","m":"[AUTO-TX] Burst done"}
```

## Лицензия

MIT
