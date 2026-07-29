/**
 * CC1101 Driver Library Implementation for ESP32-C3
 *
 * Based on SmartRC-CC1101-Driver-Lib by LSatan
 * Adapted for TPMS emulation project
 *
 * CC1101 Datasheet: http://www.ti.com/lit/ds/symlink/cc1101.pdf
 */

#include "CC1101.h"

// ============================================================================
// Constructor
// ============================================================================
CC1101::CC1101(uint8_t csPin, uint8_t gdo0Pin, uint8_t gdo2Pin) {
    _csPin = csPin;
    _gdo0Pin = gdo0Pin;
    _gdo2Pin = gdo2Pin;
    _mhzOsc = 26.0f;  // Default crystal: 26 MHz
    _currentFreq = 315.0f;
    _spiSettings = SPISettings(4000000, MSBFIRST, SPI_MODE0);
}

// ============================================================================
// SPI Communication
// ============================================================================
void CC1101::spiSelect() {
    digitalWrite(_csPin, LOW);
    delayMicroseconds(1);
}

void CC1101::spiDeselect() {
    digitalWrite(_csPin, HIGH);
    delayMicroseconds(1);
}

uint8_t CC1101::spiTransfer(uint8_t data) {
    return SPI.transfer(data);
}

void CC1101::setSpiSettings() {
    _spiSettings = SPISettings(4000000, MSBFIRST, SPI_MODE0);
}

// ============================================================================
// Low-level Register Access
// ============================================================================
uint8_t CC1101::readReg(uint8_t addr) {
    uint8_t val;
    spiSelect();
    spiTransfer(addr | 0x80);  // Read: set bit 7
    val = spiTransfer(0x00);
    spiDeselect();
    return val;
}

void CC1101::writeReg(uint8_t addr, uint8_t value) {
    spiSelect();
    spiTransfer(addr & 0x3F);  // Write: clear bit 6,7
    spiTransfer(value);
    spiDeselect();
}

uint8_t CC1101::readStatusReg(uint8_t addr) {
    uint8_t val;
    spiSelect();
    spiTransfer(addr | 0xC0);  // Status: set bit 6,7
    val = spiTransfer(0x00);
    spiDeselect();
    return val;
}

void CC1101::writeBurstReg(uint8_t addr, const uint8_t *buffer, uint8_t length) {
    spiSelect();
    spiTransfer(addr | 0x40);  // Burst write: set bit 6
    for (uint8_t i = 0; i < length; i++) {
        spiTransfer(buffer[i]);
    }
    spiDeselect();
}

void CC1101::readBurstReg(uint8_t addr, uint8_t *buffer, uint8_t length) {
    spiSelect();
    spiTransfer(addr | 0xC0);  // Burst read: set bit 6,7
    for (uint8_t i = 0; i < length; i++) {
        buffer[i] = spiTransfer(0x00);
    }
    spiDeselect();
}

void CC1101::sendCommand(uint8_t cmd) {
    spiSelect();
    spiTransfer(cmd);
    spiDeselect();
}

// ============================================================================
// Initialize CC1101
// ============================================================================
void CC1101::init() {
    // Setup CS pin
    pinMode(_csPin, OUTPUT);
    digitalWrite(_csPin, HIGH);

    // Setup GDO pins (configured as inputs)
    pinMode(_gdo0Pin, INPUT);
    pinMode(_gdo2Pin, INPUT);

    // Initialize SPI — platform-specific pin mapping
    // CS handled manually via digitalWrite
#if defined(ESP8266)
    // ESP8266 HSPI with user-defined pins:
    //   SCK=GPIO14(D5), MISO=GPIO13(D7), MOSI=GPIO12(D6)
    SPI.begin();
    SPI.pins(14, 13, 12, -1);
#elif defined(ARDUINO_ARCH_NRF52) || defined(NRF52840_XXAA) || defined(ARDUINO_NRF52_ADAFRUIT)
    // nRF52840 (ProMicro nRF52840 V1940 / Nice!Nano clone, Feather variant):
    //   MISO=D29(P0.17="017"), SCK=D21(P0.31="031"), MOSI=D20(P0.29="029")
    //   CS is handled manually via digitalWrite
    SPI.setPins(29, 21, 20);
    SPI.begin();
#else
    // ESP32-C3 FSPI: SCK=GPIO2, MISO=GPIO3, MOSI=GPIO1
    SPI.begin(2, 3, 1, -1);
#endif

    // Reset CC1101
    reset();

    // Configure GDO0 as RX/TX packet indicator
    writeReg(CC1101_IOCFG0, 0x06);  // Assert on sync word RX, de-assert on end of packet

    // Configure GDO2 as RX FIFO threshold indicator
    writeReg(CC1101_IOCFG2, 0x04);  // Assert when RX FIFO >= threshold

    // Configure IOCFG1 (optional)
    writeReg(CC1101_IOCFG1, 0x02);  // High impedance (3-state)

    // Set default register values for 315 MHz operation
    writeReg(CC1101_FSCTRL1, 0x06);  // IF frequency = 152.344 kHz
    writeReg(CC1101_FSCTRL0, 0x00);  // FREQEST

    // AGC settings
    writeReg(CC1101_AGCCTRL2, 0x00);  // Max gain: LNA=5.2dB, DVGA=0dB attenuation
    writeReg(CC1101_AGCCTRL1, 0x00);  // AGC control
    writeReg(CC1101_AGCCTRL0, 0x91);  // AGC control (hysteresis)

    // Freq offset compensation
    writeReg(CC1101_FOCCFG, 0x1D);    // Freq offset compensation config

    // Bit synchronization
    writeReg(CC1101_BSCFG, 0x1C);     // Bit sync config

    // Data rate exponent/mantissa will be set by setDRate()

    // Front-end configuration
    writeReg(CC1101_FREND1, 0xB6);    // RX front-end
    writeReg(CC1101_FREND0, 0x10);    // TX front-end

    // Frequency synthesizer calibration
    writeReg(CC1101_FSCAL3, 0xEA);    // Calibration settings
    writeReg(CC1101_FSCAL2, 0x2A);
    writeReg(CC1101_FSCAL1, 0x00);
    writeReg(CC1101_FSCAL0, 0x1F);

    // MCSM settings
    writeReg(CC1101_MCSM2, 0x07);     // RX timeout
    writeReg(CC1101_MCSM1, 0x30);     // CCA mode, RX after TX
    writeReg(CC1101_MCSM0, 0x19);     // FS_AUTOCAL=01 (cal when IDLE->TX), XOSC_FORCE_ON, PIN_CTRL_EN

    // RC oscillator
    writeReg(CC1101_RCCTRL1, 0x41);   // RC oscillator calibration
    writeReg(CC1101_RCCTRL0, 0x00);

    // Production test registers (leave at default)
    writeReg(CC1101_FSTEST, 0x59);
    writeReg(CC1101_PTEST, 0x7F);
    writeReg(CC1101_AGCTEST, 0x3F);
    writeReg(CC1101_TEST2, 0x81);
    writeReg(CC1101_TEST1, 0x35);
    writeReg(CC1101_TEST0, 0x09);

    // Set default PA table (transmit power)
    setPA(7);  // Maximum power

    // Set default frequency
    setFreq(315.0f);

    // Set default modulation
    setModulation(0);  // 2-FSK (0=2-FSK, NOT 2!)

    // Set default data rate
    setDRate(38400);

    // Set default deviation
    setDeviation(38.0f);
}

// ============================================================================
// Reset CC1101
// ============================================================================
void CC1101::reset() {
    spiDeselect();
    delayMicroseconds(5);
    spiSelect();
    delayMicroseconds(1);
    spiDeselect();
    delayMicroseconds(40);

    // Send SRES command
    sendCommand(CC1101_SRES);

    // Wait for reset to complete
    delayMicroseconds(100);

    // Verify chip is responsive (silent — no Serial debug)
    uint8_t partnum = readStatusReg(CC1101_PARTNUM);
    uint8_t version = readStatusReg(CC1101_VERSION);

    (void)partnum;
    (void)version;
    // Chip verification removed — no Serial output in production build
}

// ============================================================================
// Wake up from power-down
// ============================================================================
void CC1101::wakeUp() {
    spiSelect();
    delayMicroseconds(1);
    spiDeselect();
    delayMicroseconds(1);
}

// ============================================================================
// Set Carrier Frequency
// ============================================================================
bool CC1101::setFreq(float frequency) {
    // f_carrier = (FREQ / 2^16) * f_xosc
    // FREQ = f_carrier * 2^16 / f_xosc

    _currentFreq = frequency;
    uint32_t freq = (uint32_t)(frequency * 65536.0f / _mhzOsc);

    writeReg(CC1101_FREQ2, (uint8_t)(freq >> 16));
    writeReg(CC1101_FREQ1, (uint8_t)(freq >> 8));
    writeReg(CC1101_FREQ0, (uint8_t)(freq & 0xFF));

    return true;
}

// ============================================================================
// Configure registers for specific frequency band
// ============================================================================
void CC1101::setFreqConfig(float frequency) {
    _currentFreq = frequency;

    if (frequency >= 400.0f) {
        // 433 MHz band configuration
        writeReg(CC1101_FSCTRL1, 0x08);   // IF = 203.125 kHz
        writeReg(CC1101_FSCTRL0, 0x00);
        writeReg(CC1101_AGCCTRL2, 0x07);  // AGC for 433 MHz
        writeReg(CC1101_AGCCTRL1, 0x00);
        writeReg(CC1101_AGCCTRL0, 0x92);
        writeReg(CC1101_FOCCFG, 0x1D);
        writeReg(CC1101_BSCFG, 0x1C);
        writeReg(CC1101_FREND1, 0x56);    // RX front-end for 433 MHz
        writeReg(CC1101_FREND0, 0x10);
        writeReg(CC1101_FSCAL3, 0xE9);    // Calibration for 433 MHz
        writeReg(CC1101_FSCAL2, 0x2A);
        writeReg(CC1101_FSCAL1, 0x00);
        writeReg(CC1101_FSCAL0, 0x1F);
    } else {
        // 315 MHz band configuration (default)
        writeReg(CC1101_FSCTRL1, 0x06);   // IF = 152.344 kHz
        writeReg(CC1101_FSCTRL0, 0x00);
        writeReg(CC1101_AGCCTRL2, 0x00);  // Max gain: LNA=5.2dB, DVGA=0dB attenuation
        writeReg(CC1101_AGCCTRL1, 0x00);  // relative CS, minimum threshold above noise floor
        writeReg(CC1101_AGCCTRL0, 0x91);
        writeReg(CC1101_FOCCFG, 0x1D);
        writeReg(CC1101_BSCFG, 0x1C);
        writeReg(CC1101_FREND1, 0xB6);    // RX front-end for 315 MHz
        writeReg(CC1101_FREND0, 0x10);
        writeReg(CC1101_FSCAL3, 0xEA);    // Calibration for 315 MHz
        writeReg(CC1101_FSCAL2, 0x2A);
        writeReg(CC1101_FSCAL1, 0x00);
        writeReg(CC1101_FSCAL0, 0x1F);
    }
}

// ============================================================================
// Set Data Rate (baud)
// ============================================================================
bool CC1101::setDRate(uint32_t baud) {
    // CC1101 datasheet formula:
    //   DRATE [baud] = (256 + DRATE_M) * 2^(DRATE_E) * f_XOSC / 2^28
    // Target: (256 + M) * 2^E = baud * 2^28 / f_XOSC
    uint32_t drate = (uint32_t)((uint64_t)baud * 268435456ULL / (_mhzOsc * 1000000));

    // Find E and M where (256+M)*2^E = drate, M in [0..255]
    uint8_t e = 0;
    uint32_t m = 0;
    for (e = 0; e <= 15; e++) {
        uint32_t scaled = (e == 0) ? drate : (drate >> e);
        if (scaled >= 256) {
            m = scaled - 256;
            if (m <= 255) break;
        }
    }

    // FIX: DRATE_E goes in bits 3:0 of MDMCFG4 (not bits 7:4!)
    // Bits 7:4 = CHANBW_E (channel bandwidth exponent) — must be preserved!
    uint8_t mdmcfg4 = readReg(CC1101_MDMCFG4);
    writeReg(CC1101_MDMCFG4, (mdmcfg4 & 0xF0) | (e & 0x0F));
    writeReg(CC1101_MDMCFG3, (uint8_t)m);

    return true;
}

// ============================================================================
// Set Modulation Format
// ============================================================================
bool CC1101::setModulation(uint8_t mod) {
    // 0 = 2-FSK  <-- we need this for TPMS!
    // 1 = GFSK
    // 2 = ASK/OOK
    // 3 = 4-FSK
    // 4 = MSK
    if (mod > 7) return false;

    uint8_t mdmcfg2 = readReg(CC1101_MDMCFG2);
    // MOD_FORMAT is bits 6:4 of MDMCFG2, SYNC_MODE is bits 2:0
    mdmcfg2 = (mdmcfg2 & 0x8F) | ((mod & 0x07) << 4);
    writeReg(CC1101_MDMCFG2, mdmcfg2);

    return true;
}

// ============================================================================
// Set Frequency Deviation
// ============================================================================
bool CC1101::setDeviation(float khz) {
    // CC1101 formula: deviation = (f_XOSC / 2^17) * (8 + DEVIATION_M) * 2^DEVIATION_E
    // Rearranging: (8 + DEVIATION_M) * 2^DEVIATION_E = deviation_Hz * 2^17 / f_XOSC_Hz
    // DEVIATION_M in [0..15], DEVIATION_E in [0..7]

    float deviation_hz = khz * 1000.0f;
    float target = deviation_hz * 131072.0f / (_mhzOsc * 1e6f);

    uint8_t e = 0;
    uint8_t m = 0;
    bool found = false;

    for (e = 0; e <= 7; e++) {
        float m_val = target / (1 << e) - 8.0f;
        if (m_val >= 0.0f && m_val <= 15.5f) {
            m = (uint8_t)(m_val + 0.5f);  // Round to nearest
            if (m > 15) m = 15;
            found = true;
            break;
        }
    }

    if (!found) {
        // Fallback: use largest valid values
        e = 7;
        m = 15;
    }

    writeReg(CC1101_DEVIATN, (e << 4) | m);
    return true;
}

// ============================================================================
// Set TX Power (PA Table)
// ============================================================================
void CC1101::setPA(int8_t index) {
    // PA table has 8 entries for different power levels
    // Power levels: -30, -20, -15, -10, 0, 5, 7, 10 dBm (approximate)
    const uint8_t pa_table_315[] = {
        0x00,  // -30 dBm
        0x03,  // -20 dBm
        0x0D,  // -15 dBm
        0x1C,  // -10 dBm
        0x34,  // 0 dBm
        0x60,  // 5 dBm
        0x84,  // 7 dBm
        0xC0   // 10 dBm (maximum for 315 MHz)
    };

    const uint8_t pa_table_433[] = {
        0x00,  // -30 dBm
        0x03,  // -20 dBm
        0x0E,  // -15 dBm
        0x1E,  // -10 dBm
        0x34,  // 0 dBm
        0x65,  // 5 dBm
        0x8B,  // 7 dBm
        0xC5   // 10 dBm (maximum for 433 MHz)
    };

    uint8_t pa_idx = constrain(index, 0, 7);
    const uint8_t* pa_table = (_currentFreq >= 400.0f) ? pa_table_433 : pa_table_315;
    uint8_t power = pa_table[pa_idx];

    // CRITICAL FIX: ALL PA table entries must be set to the SAME power value.
    // CC1101 uses PATABLE[0] for TX output (when FREND0 PA_POWER=0, default).
    // Previous code set PATABLE[0]=pa_table_315[0]=0x00 (PA OFF!) regardless
    // of the selected index. This caused ZERO RF output!
    uint8_t pa_values[8];
    for (int i = 0; i < 8; i++) {
        pa_values[i] = power;
    }

    // Write the PA table starting at PA_TABLE0 (0x3E)
    spiSelect();
    spiTransfer(0x3E | 0x40);  // Burst write to PA table
    for (int i = 0; i < 8; i++) {
        spiTransfer(pa_values[i]);
    }
    spiDeselect();
}

// ============================================================================
// Set Sync Word
// ============================================================================
void CC1101::setSyncWord(uint8_t syncH, uint8_t syncL) {
    writeReg(CC1101_SYNC1, syncH);
    writeReg(CC1101_SYNC0, syncL);
}

// ============================================================================
// Set Sync Mode
// ============================================================================
void CC1101::setSyncMode(uint8_t mode) {
    // SYNC_MODE values (CC1101 datasheet, bits 2:0 of MDMCFG2):
    // 0 = No preamble/sync
    // 1 = 15/16 sync word bits
    // 2 = 16/16 sync word bits
    // 3 = 30/32 sync word bits
    // 4 = No preamble/sync (CS required)
    uint8_t mdmcfg2 = readReg(CC1101_MDMCFG2);
    mdmcfg2 = (mdmcfg2 & 0xF8) | (mode & 0x07);
    writeReg(CC1101_MDMCFG2, mdmcfg2);
}

// ============================================================================
// Set Packet Length
// ============================================================================
void CC1101::setPktLength(uint8_t length) {
    writeReg(CC1101_PKTLEN, length);
}

// ============================================================================
// Set CRC Enable
// ============================================================================
void CC1101::setCrc(bool enable) {
    uint8_t pktctrl0 = readReg(CC1101_PKTCTRL0);
    if (enable) {
        pktctrl0 |= 0x04;  // Enable CRC
    } else {
        pktctrl0 &= ~0x04;  // Disable CRC
    }
    writeReg(CC1101_PKTCTRL0, pktctrl0);
}

// ============================================================================
// Set Data Whitening
// ============================================================================
void CC1101::setWhiteData(bool enable) {
    uint8_t pktctrl0 = readReg(CC1101_PKTCTRL0);
    if (enable) {
        pktctrl0 |= 0x40;  // Enable whitening
    } else {
        pktctrl0 &= ~0x40;  // Disable whitening
    }
    writeReg(CC1101_PKTCTRL0, pktctrl0);
}

// ============================================================================
// Set Manchester Encoding
// ============================================================================
void CC1101::setManc(uint8_t enable) {
    // Manchester encoding is in MDMCFG2 bit 3, NOT PKTCTRL1!
    uint8_t mdmcfg2 = readReg(CC1101_MDMCFG2);
    if (enable) {
        mdmcfg2 |= 0x08;  // Enable Manchester (bit 3 of MDMCFG2)
    } else {
        mdmcfg2 &= ~0x08;  // Disable Manchester
    }
    writeReg(CC1101_MDMCFG2, mdmcfg2);
}

// ============================================================================
// Set Preamble Quality Threshold
// ============================================================================
void CC1101::setPqt(uint8_t pqt) {
    uint8_t pktctrl1 = readReg(CC1101_PKTCTRL1);
    pktctrl1 = (pktctrl1 & 0x8F) | ((pqt & 0x07) << 4);
    writeReg(CC1101_PKTCTRL1, pktctrl1);
}

// ============================================================================
// Set Crystal Oscillator Frequency
// ============================================================================
void CC1101::setMHZOsc(float mhz) {
    _mhzOsc = mhz;
}

// ============================================================================
// Set Device Address
// ============================================================================
void CC1101::setAddr(uint8_t addr) {
    writeReg(CC1101_ADDR, addr);
}

// ============================================================================
// Set Channel Number
// ============================================================================
void CC1101::setChannr(uint8_t ch) {
    writeReg(CC1101_CHANNR, ch);
}

// ============================================================================
// Transmit Data
// ============================================================================
bool CC1101::sendData(const uint8_t *data, uint8_t length) {
    // Flush TX FIFO first
    flushTxFifo();

    // Write data to TX FIFO
    writeBurstReg(CC1101_TX_FIFO, data, length);

    // Enter TX mode
    setTxState();

    // Wait for TX to complete (GDO0 goes low when done)
    unsigned long start = millis();
    while (digitalRead(_gdo0Pin) == HIGH) {
        if (millis() - start > 500) {
            setIdleState();
            return false;
        }
        // No yield() on ESP32 — proper RTOS scheduling
    }

    // Return to IDLE
    setIdleState();
    return true;
}

// ============================================================================
// Set TX State
// ============================================================================
void CC1101::setTxState() {
    sendCommand(CC1101_STX);
    // Wait for calibration to complete
    delay(1);
}

// ============================================================================
// Set RX State
// ============================================================================
void CC1101::setRxState() {
    sendCommand(CC1101_SRX);
    delay(1);
}

// ============================================================================
// Configure CC1101 for TPMS Reception
// ============================================================================
void CC1101::setRxConfig() {
    // NOTE: Band-specific registers (FSCTRL, AGCCTRL, FOCCFG, BSCFG, FREND, FSCAL)
    // are set by setFreqConfig(). This function only sets packet-related registers.
    
    // IOCFG2: High impedance
    writeReg(CC1101_IOCFG2, 0x2E);
    // IOCFG0: Asserts when RX FIFO is above threshold
    writeReg(CC1101_IOCFG0, 0x06);
    // IOCFG1: High impedance
    writeReg(CC1101_IOCFG1, 0x2E);
    // FIFOTHR: 64 byte TX FIFO, 61 byte RX FIFO
    writeReg(CC1101_FIFOTHR, 0x47);
    // PKTCTRL1: No address check, no CRC auto-flush
    writeReg(CC1101_PKTCTRL1, 0x00);
    // PKTCTRL0: Infinite packet length mode (bits 1:0=10), no CRC, no append status
    writeReg(CC1101_PKTCTRL0, 0x02);
    // PKTLEN: don't-care in infinite length mode
    writeReg(CC1101_PKTLEN, 0x00);
    // MDMCFG4: CHANBW_E=1, CHANBW_M=3 -> ~232 kHz channel bandwidth
    // DRATE_E will be set by setDRate()
    writeReg(CC1101_MDMCFG4, 0x7D);
    // MDMCFG2: 2-FSK, no preamble/sync, no carrier sense, no Manchester
    writeReg(CC1101_MDMCFG2, 0x00);
    // MDMCFG1: FEC disabled, 4 preamble bytes
    writeReg(CC1101_MDMCFG1, 0x23);
    // MDMCFG0: Default
    writeReg(CC1101_MDMCFG0, 0xF8);
    // MCSM0: Auto-calibrate
    writeReg(CC1101_MCSM0, 0x18);
    // MCSM1: Stay in RX after packet received (RXOFF_MODE=11=Stay in RX)
    writeReg(CC1101_MCSM1, 0x30);
    // MCSM2: No RX timeout
    writeReg(CC1101_MCSM2, 0x00);
    // TEST
    writeReg(CC1101_TEST2, 0x81);
    writeReg(CC1101_TEST1, 0x35);
    writeReg(CC1101_TEST0, 0x09);
    // SYNC word: match first 16 settling zeros for re-sync after flush
    writeReg(CC1101_SYNC1, 0x00);
    writeReg(CC1101_SYNC0, 0x00);
}

// ============================================================================
// Set IDLE State
// ============================================================================
void CC1101::setIdleState() {
    sendCommand(CC1101_SIDLE);
    delay(1);
}

// ============================================================================
// Flush TX FIFO
// ============================================================================
void CC1101::flushTxFifo() {
    sendCommand(CC1101_SFTX);
}

// ============================================================================
// Flush RX FIFO
// ============================================================================
void CC1101::flushRxFifo() {
    sendCommand(CC1101_SFRX);
}

// ============================================================================
// Receive Data
// ============================================================================
bool CC1101::receiveData(uint8_t *data, uint8_t *length) {
    uint8_t rxBytes = getRxBytes() & 0x7F;  // Mask off overflow bit

    if (rxBytes == 0) return false;

    *length = rxBytes;
    readBurstReg(CC1101_RX_FIFO, data, rxBytes);

    // Flush RX FIFO
    flushRxFifo();

    return true;
}

// ============================================================================
// Get Chip State (from MARCSTATE)
// ============================================================================
uint8_t CC1101::getChipState() {
    return readStatusReg(CC1101_MARCSTATE) & 0x1F;
}

// ============================================================================
// Get RSSI (dBm, approximate)
// ============================================================================
int8_t CC1101::getRssi() {
    int8_t rssi = (int8_t)readStatusReg(CC1101_RSSI);
    if (rssi >= 128) {
        return ((int16_t)rssi - 256) / 2 - 74;
    } else {
        return (int16_t)rssi / 2 - 74;
    }
}

// ============================================================================
// Get LQI
// ============================================================================
uint8_t CC1101::getLqi() {
    return readStatusReg(CC1101_LQI);
}

// ============================================================================
// Get MARC State Machine State
// ============================================================================
uint8_t CC1101::getMarcState() {
    return getChipState();
}

// ============================================================================
// Get TX FIFO Bytes
// ============================================================================
uint8_t CC1101::getTxBytes() {
    return readStatusReg(CC1101_TXBYTES);
}

// ============================================================================
// Get RX FIFO Bytes
// ============================================================================
uint8_t CC1101::getRxBytes() {
    return readStatusReg(CC1101_RXBYTES);
}

// ============================================================================
// Print All Registers (Debug — no-op in production build)
// ============================================================================
void CC1101::printRegs() {
    // Debug function removed for production build (no Serial output)
}
