# TPMS Emulator / Sniffer v7.3

ESP32-C3 + CC1101 TPMS sniffer and ESP8266 + CC1101 TPMS emulator for **Acura RDX 2008** (PMV-107J sensors, 315 MHz).

![ESP32-C3](ESP32-C3-super-mini.jpg)

## Features

- **4-sensor emulator** — simulates PMV-107J TPMS sensors (315 MHz)
- **Real sensor sniffing** — receives and decodes genuine PMV-107J transmissions
- **Web interface** — configure all parameters from your phone/PC
- **OTA updates** — flash firmware over WiFi without USB
- **Battery powered** — deep sleep with voltage monitoring (18650 Li-Ion)
- **License system** — 24h trial, then license key required

## Hardware

### Parts List

| Part | Qty | Description | Price |
|------|-----|-------------|-------|
| ESP32-C3 Mini DevKit | 1 | Sniffer board | ~300₽ |
| ESP8266 NodeMCU | 1 | Emulator board | ~150₽ |
| CC1101 module (315 MHz) | 2 | RF transceiver with spring antenna | ~100₽ |
| TP4056 module | 2 | 18650 charger (1A) | ~50₽ |
| DW01 + FS8205 | 2 | Battery protection | ~10₽ |
| AMS1117-3.3 | 2 | 3.3V LDO regulator | ~20₽ |
| 10kΩ resistors | 4 | Voltage divider (2 per board) | ~5₽ |
| 18650 holder | 2 | Battery holder | ~30₽ |
| 18650 battery | 2 | 3000mAh Li-Ion (4.2V) | ~300₽ |

### ESP32-C3 Mini → CC1101 Wiring

```
ESP32-C3 Pin        →  CC1101 Pin       Function
──────────────────────────────────────────────────────
GPIO6               →  Pin 1 (VCC)      Power (direct from GPIO!)
GND                 →  Pin 2 (GND)      Ground
GPIO1 (FSPI MOSI)   →  Pin 3 (SI/DIN)   SPI MOSI
GPIO3 (FSPI MISO)   →  Pin 4 (SO/DOUT)  SPI MISO
GPIO2 (FSPI SCK)    →  Pin 5 (CLK)      SPI Clock
GPIO7               →  Pin 6 (CSN)      Chip Select
GPIO4               →  Pin 7 (GDO0)     GDO0
GPIO5               →  Pin 8 (GDO2)     GDO2
```

### ESP8266 NodeMCU → CC1101 Wiring

```
ESP8266 Pin         →  CC1101 Pin       Function
──────────────────────────────────────────────────────
D4 (GPIO2)          →  VCC via N-MOSFET  Power (via MOSFET gate)
GND                 →  Pin 2 (GND)      Ground
D6 (GPIO12 / HSPI)  →  Pin 3 (SI/DIN)   SPI MOSI
D7 (GPIO13 / HSPI)  →  Pin 4 (SO/DOUT)  SPI MISO
D5 (GPIO14 / HSPI)  →  Pin 5 (CLK)      SPI Clock
D8 (GPIO15)         →  Pin 6 (CSN)      Chip Select
D1 (GPIO5)          →  Pin 7 (GDO0)     GDO0
D2 (GPIO4)          →  Pin 8 (GDO2)     GDO2
```

### Battery Voltage Divider (Both Boards)

```
18650 (3.0-4.2V) → [10kΩ] → ADC pin → [10kΩ] → GND

Formula: Vbat = ADC_reading × 2

Vbat    Vout    Status
──────────────────────
4.2V    2.1V    Full charge
3.7V    1.85V   Nominal
3.3V    1.65V   LOW warning
3.0V    1.5V    CRITICAL — device shuts down
```

### Deep Sleep Wiring

- **ESP8266**: Solder D0 (GPIO16) → RST (required for wake from deep sleep)
- **ESP32-C3**: Timer wake (no extra wiring needed)

### Power Circuit (Per Board)

```
[18650] → [TP4056] → [DW01+FS8205] → [AMS1117-3.3] → [ESP] → [CC1101]
 3.7V     charger    over-discharge    3.3V output
```

## Building

### Requirements

- [PlatformIO](https://platformio.org/) (VS Code plugin or CLI)
- USB cable (for initial flash only)

### Build Commands

```bash
# ESP32-C3 sniffer
pio run -e esp32-c3-devkitm-1

# ESP8266 emulator
pio run -e esp8266-nodemcu
```

### Flash Commands

```bash
# ESP32-C3 (COM3)
pio run -e esp32-c3-devkitm-1 -t upload --upload-port COM3

# ESP8266 (COM8)
pio run -e esp8266-nodemcu -t upload --upload-port COM8
```

> **Note**: ESP32-C3 USB-Serial/JTAG requires pressing the physical RST button after flashing.

## Usage

### Web Interface

1. Power on the device
2. Connect to WiFi: **TPMS** / `12345678`
3. Open browser: `http://192.168.4.1`

### Serial Commands

| Command | Description |
|---------|-------------|
| `n` | Toggle sniffer start/stop |
| `s` | Wide frequency sweep |
| `t` | Self-echo test |
| `r` | Reset trial timer |

### Behavior

- **First 3 minutes**: WiFi AP active, web interface accessible
- **After 3 min (no connection)**: Device enters deep sleep between transmissions
- **On wake**: Send burst → deep sleep (no WiFi)
- **Battery critical (< 3.0V)**: Device stops transmitting to protect battery

## Protocol — PMV-107J

| Parameter | Value |
|-----------|-------|
| Modulation | 2-FSK (mod=0) |
| Data rate | 10 kbps |
| Deviation | 38 kHz |
| Encoding | Differential Manchester |
| Preamble | 0xFC (11111100, 8 bits) |
| Packet | 8-bit preamble + 68 DM bits = 144 on-air bits |
| Frequency | 315.000 MHz |

### Packet Structure

```
[8-bit preamble][68 DM-encoded bits]
  ↓
[28-bit ID][1-bit bat][2-bit cnt][1-bit 0][1-bit rapid][1-bit fail]
[8-bit P][8-bit !P][8-bit T][8-bit CRC]
```

CRC-8: poly=0x13, init=0x00, over first 58 bits

## License System

- **Trial**: 24 hours (resets on each flash)
- **License key**: 8 hex characters
- **ESP8266**: Any valid hex key accepted
- **ESP32-C3**: HMAC-SHA256 verification
- **Trial expiration**: CC1101 power cut, device enters deep sleep
- **Contact**: wargaelanor@yandex.ru

## Battery Life Estimate

| Mode | Avg Current | Life (3000mAh) |
|------|-------------|-----------------|
| ESP8266 5min interval | ~1.5 mA | **~83 days** |
| ESP32-C3 duty-cycle | ~40 mA | **~75 hours** |

## Project Structure

```
TPMS-Emulator/
├── src/
│   └── main.cpp          # All firmware code (emulator + sniffer)
├── lib/
│   └── CC1101_Driver/    # CC1101 SPI driver
├── release/
│   ├── firmware_esp32c3.bin   # Pre-built ESP32-C3 firmware
│   ├── firmware_esp8266.bin   # Pre-built ESP8266 firmware
│   ├── flash.bat              # Flash utility
│   ├── TPMS_Tool.exe          # PC tool
│   └── pinout.txt             # Wiring reference
├── data/
│   └── index.html             # Web interface source
├── platformio.ini             # Build configuration
└── AGENTS.md                  # Developer notes
```

## Release Files

| File | Description |
|------|-------------|
| `firmware_esp32c3.bin` | ESP32-C3 sniffer firmware (flash to COM3) |
| `firmware_esp8266.bin` | ESP8266 emulator firmware (flash to COM8) |
| `flash.bat` | Windows flash utility |
| `TPMS_Tool.exe` | PC tool for configuration |
| `pinout.txt` | Complete wiring reference |

## Tested Sensor IDs

| ID | Source | Notes |
|----|--------|-------|
| 00001111 | ESP8266 emulator | Sensor #4 |
| 00002222 | ESP8266 emulator | Sensor #1 |
| 00003333 | ESP8266 emulator | Sensor #2 |
| 00004444 | ESP8266 emulator | Sensor #3 |
| 02c03839 | Real PMV-107J | RSSI=-83..-93 dBm |

## License

MIT
