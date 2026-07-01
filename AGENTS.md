# AGENTS.md — TPMS Sniffer/Emulator Project

## Overview
ESP32-C3 + CC1101 TPMS sniffer and ESP8266 + CC1101 TPMS emulator for Acura RDX 2008 (PMV-107J sensors, 315 MHz).

## Hardware
- **ESP32-C3 Mini** — sniffer (CC1101 on SPI, 315 MHz). COM3.
- **ESP8266 NodeMCU** — emulator (CC1101 on SPI, 315 MHz). COM8. Powered from charger.
- Both use CC1101 modules with spring antennas.

## Sensor Protocol (PMV-107J)
- **Modulation**: 2-FSK (mod=0), no Manchester hardware (setManc=0)
- **Data rate**: 10 kbps (DRATE=0x59, MDRATE3=0x08)
- **Deviation**: 38 kHz (DEVIATN=0x47)
- **Encoding**: Differential Manchester (software decoded via `dm_decode`)
- **Preamble**: 0xFC (11111100, 8 bits)
- **Packet**: 8-bit preamble + 68 DM-encoded bits = 144 on-air bits at 10 kbps
- **Frequency**: 315.000 MHz (FREQ0=0x21, FREQ1=0x62, FREQ2=0x10)
- **RX config**: PKTCTRL0=0x02 (infinite length), SYNC_MODE=0 (no sync), no CRC

## Key CC1101 Settings (from lib/CC1101_Driver/CC1101.cpp)
- MDMCFG2: MOD_FORMAT=0x00 (2-FSK), SYNC_MODE=0x00 (no sync/preamble)
- PKTCTRL0: LENGTH_CONFIG=0x02 (infinite), no CRC, no whitening
- MCSM1: 0x30 (no CCA, RX after TX)
- AGCCTRL1: 0x00, AGCCTRL2: 0x00 (default gain)
- MARCSTATE 0x0D = RX mode

## Build Commands
```bash
# ESP32-C3 sniffer
pio run -e esp32-c3-devkitm-1 -t upload --upload-port COM3

# ESP8266 emulator
pio run -e esp8266-nodemcu -t upload --upload-port COM8
```

## Serial Commands
- `n` — toggle sniffer start/stop
- `s` — frequency sweep
- `t` — self-echo test
- `r` — reset trial timer (24h block)

## Web Interface
- ESP8266 AP: `TPMS_v1` / `12345678`
- ESP32 AP: `TPMS` / `12345678`
- Web UI shows decoded sensors with ID, pressure, temperature, RSSI

## Tested Sensor IDs
| ID | Source | Notes |
|---|---|---|
| 00001111 | ESP8266 emulator | Sensor #4 |
| 00002222 | ESP8266 emulator | Sensor #1 |
| 00003333 | ESP8266 emulator | Sensor #2 |
| 00004444 | ESP8266 emulator | Sensor #3 |
| 02c03839 | Real PMV-107J | RSSI=-83..-93 dBm, ~17s interval |

## Key Implementation Notes
- CC1101 stays in RX continuously (no duty-cycling) for real sensor reception
- Reverse preamble search with >=136 bit requirement (real sensor has 136 NRZ bits after preamble)
- RSSI threshold: -95 dBm for decode attempts
- MARC state auto-recovery: if CC1101 leaves RX, restart SCAL+RX
- WiFi stays ON during sniffer for web UI access
- ESP8266: `autoTxInterval` forced to 5s in setup
- ESP8266: `enterDeepSleep()` restarts WiFi AP + reinitializes CC1101
- Trial timer: blocks sniffer after 24h, reset with 'r' command
- ESP32-C3 USB-Serial/JTAG requires physical RST button after flash

## License System
- **Trial**: 24 hours (86400 seconds), tracked in EEPROM[43-46]
- **License byte**: EEPROM[42] — 0=unlicensed, 0xFF=licensed
- **ESP32-C3**: HMAC-SHA256 verification (secret key + device MAC → 8 hex chars)
- **ESP8266**: Simple 8-char hex key format check (no mbedTLS available)
- **Trial expiration**: CC1101 power is CUT immediately (GPIO6 LOW on ESP32, GPIO2 LOW on ESP8266)
- **Auto-TX blocked**: No transmission when trial expired
- **Web UI**: Shows license status, allows key entry
- **Reset trial**: Serial command `'r'` or EEPROM write

## Battery Power System
- **Voltage divider**: 10kΩ + 10kΩ on both boards (A0/GPIO0)
- **ADC formula**: Vbat = ADC_reading * 2
- **Thresholds**: BATT_FULL=4200mV, BATT_LOW=3300mV, BATT_CRITICAL=3000mV
- **EEPROM[47-48]**: battMah (uint16 LE, default 3000)
- **Deep sleep**: Both boards use `ESP.deepSleep()` (ESP8266 needs GPIO16→RST wire)
- **Wake behavior**: Send burst → deep sleep (NO WiFi on wake)
- **Critical protection**: Device enters deep sleep without TX if battery < 3.0V
- **Web UI**: Battery indicator (mV + percentage) in header
- **Estimated life**: ~18 days at 5min interval with 3000mAh battery

## Release Files
- `release/firmware_esp32c3.bin` — latest ESP32-C3 sniffer firmware
- `release/firmware_esp8266.bin` — latest ESP8266 emulator firmware
- `release/flash.bat` — flash utility
- `release/TPMS_Tool.exe` — PC tool
- `release/pinout.txt` — CC1101 wiring reference for both boards
