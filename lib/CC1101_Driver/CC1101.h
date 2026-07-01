/**
 * CC1101 Driver Library for ESP32-C3
 *
 * Based on SmartRC-CC1101-Driver-Lib by LSatan
 * Adapted for TPMS emulation project
 *
 * CC1101 Register Map and Command Definitions
 */

#ifndef CC1101_H
#define CC1101_H

#include <Arduino.h>
#include <SPI.h>

// ============================================================================
// CC1101 SPI Command Strobes (0x30-0x3D)
// ============================================================================
#define CC1101_SRES           0x30   // Reset chip
#define CC1101_SFSTXON        0x31   // Enable and calibrate frequency synthesizer
#define CC1101_SXOFF          0x32   // Turn off crystal oscillator
#define CC1101_SCAL           0x33   // Calibrate frequency synthesizer
#define CC1101_SRX            0x34   // Enable RX
#define CC1101_STX            0x35   // Enable TX
#define CC1101_SIDLE          0x36   // Exit RX/TX, enter IDLE
#define CC1101_SWOR           0x38   // Start automatic RX polling
#define CC1101_SPWD           0x39   // Enter power-down mode
#define CC1101_SFRX           0x3A   // Flush RX FIFO
#define CC1101_SFTX           0x3B   // Flush TX FIFO
#define CC1101_SWORRST        0x3C   // Reset real-time clock
#define CC1101_SNOP           0x3D   // No operation

// ============================================================================
// CC1101 Configuration Registers (0x00-0x2E)
// ============================================================================
#define CC1101_IOCFG0         0x00   // GDO0 output pin configuration
#define CC1101_IOCFG1         0x01   // GDO1 output pin configuration
#define CC1101_IOCFG2         0x02   // GDO2 output pin configuration
#define CC1101_FIFOTHR        0x03   // RX FIFO and TX FIFO thresholds
#define CC1101_SYNC1          0x04   // Sync word, high byte
#define CC1101_SYNC0          0x05   // Sync word, low byte
#define CC1101_PKTLEN         0x06   // Packet length
#define CC1101_PKTCTRL1       0x07   // Packet automation control
#define CC1101_PKTCTRL0       0x08   // Packet automation control
#define CC1101_ADDR           0x09   // Device address
#define CC1101_CHANNR         0x0A   // Channel number
#define CC1101_FSCTRL1        0x0B   // Frequency synthesizer control
#define CC1101_FSCTRL0        0x0C   // Frequency synthesizer control
#define CC1101_FREQ2          0x0D   // Frequency control word, high byte
#define CC1101_FREQ1          0x0E   // Frequency control word, middle byte
#define CC1101_FREQ0          0x0F   // Frequency control word, low byte
#define CC1101_MDMCFG4        0x10   // Modem configuration
#define CC1101_MDMCFG3        0x11   // Modem configuration
#define CC1101_MDMCFG2        0x12   // Modem configuration
#define CC1101_MDMCFG1        0x13   // Modem configuration
#define CC1101_MDMCFG0        0x14   // Modem configuration
#define CC1101_DEVIATN        0x15   // Modem deviation setting
#define CC1101_MCSM2          0x16   // Main Radio Control State Machine config
#define CC1101_MCSM1          0x17   // Main Radio Control State Machine config
#define CC1101_MCSM0          0x18   // Main Radio Control State Machine config
#define CC1101_FOCCFG         0x19   // Frequency Offset Compensation config
#define CC1101_BSCFG          0x1A   // Bit Synchronization configuration
#define CC1101_AGCCTRL2       0x1B   // AGC control
#define CC1101_AGCCTRL1       0x1C   // AGC control
#define CC1101_AGCCTRL0       0x1D   // AGC control
#define CC1101_WOREVT1        0x1E   // High byte Event 0 timeout
#define CC1101_WOREVT0        0x1F   // Low byte Event 0 timeout
#define CC1101_WORCTRL        0x20   // Wake On Radio control
#define CC1101_FREND1         0x21   // Front end RX configuration
#define CC1101_FREND0         0x22   // Front end TX configuration
#define CC1101_FSCAL3         0x23   // Frequency synthesizer calibration
#define CC1101_FSCAL2         0x24   // Frequency synthesizer calibration
#define CC1101_FSCAL1         0x25   // Frequency synthesizer calibration
#define CC1101_FSCAL0         0x26   // Frequency synthesizer calibration
#define CC1101_RCCTRL1        0x27   // RC oscillator configuration
#define CC1101_RCCTRL0        0x28   // RC oscillator configuration
#define CC1101_FSTEST         0x29   // Frequency synthesizer calibration control
#define CC1101_PTEST          0x2A   // Production test
#define CC1101_AGCTEST        0x2B   // AGC test
#define CC1101_TEST2          0x2C   // Various test settings
#define CC1101_TEST1          0x2D   // Various test settings
#define CC1101_TEST0          0x2E   // Various test settings

// ============================================================================
// CC1101 Status Registers (0x30-0x3D, read-only)
// ============================================================================
#define CC1101_PARTNUM        0x30   // Chip ID
#define CC1101_VERSION        0x31   // Chip version
#define CC1101_RSSI           0x34   // Received signal strength indication
#define CC1101_MARCSTATE      0x35   // Main Radio Control State Machine state
#define CC1101_WORTIME1       0x36   // High byte of WOR timer
#define CC1101_WORTIME0       0x37   // Low byte of WOR timer
#define CC1101_FREQEST        0x38   // Frequency offset estimate (signed)
#define CC1101_LQI            0x39   // Demodulator estimate for link quality
#define CC1101_TXBYTES        0x3A   // Underflow flag + number of bytes in TX FIFO
#define CC1101_RXBYTES        0x3B   // Overflow flag + number of bytes in RX FIFO
#define CC1101_CRCSTAT        0x3B   // CRC check status
#define CC1101_TX_FIFO        0x3F   // TX FIFO access
#define CC1101_RX_FIFO        0x3F   // RX FIFO access

// ============================================================================
// CC1101 Class
// ============================================================================
class CC1101 {
public:
    CC1101(uint8_t csPin = 7, uint8_t gdo0Pin = 4, uint8_t gdo2Pin = 5);

    void     init();
    void     reset();
    void     wakeUp();
    void     setSpiSettings();

    // --- Configuration methods ---
    bool     setFreq(float frequency);
    void     setFreqConfig(float frequency);
    bool     setDRate(uint32_t baud);
    bool     setModulation(uint8_t mod);
    bool     setDeviation(float khz);
    void     setPA(int8_t index);
    void     setSyncWord(uint8_t syncH, uint8_t syncL);
    void     setSyncMode(uint8_t mode);
    void     setPktLength(uint8_t length);
    void     setCrc(bool enable);
    void     setWhiteData(bool enable);
    void     setManc(uint8_t enable);
    void     setPqt(uint8_t pqt);
    void     setMHZOsc(float mhz);
    void     setAddr(uint8_t addr);
    void     setChannr(uint8_t ch);

    // --- Transmit/Receive methods ---
    bool     sendData(const uint8_t *data, uint8_t length);
    void     setTxState();
    void     setRxState();
    void     setIdleState();
    void     flushTxFifo();
    void     flushRxFifo();
    bool     receiveData(uint8_t *data, uint8_t *length);
    void     setRxConfig();

    // --- Status methods ---
    uint8_t  getChipState();
    int8_t   getRssi();
    uint8_t  getLqi();
    uint8_t  getMarcState();
    uint8_t  getTxBytes();
    uint8_t  getRxBytes();

    // --- Debug methods ---
    void     printRegs();

    // --- Low-level register access ---
    uint8_t  readReg(uint8_t addr);
    void     writeReg(uint8_t addr, uint8_t value);
    uint8_t  readStatusReg(uint8_t addr);
    void     writeBurstReg(uint8_t addr, const uint8_t *buffer, uint8_t length);
    void     readBurstReg(uint8_t addr, uint8_t *buffer, uint8_t length);
    void     sendCommand(uint8_t cmd);

private:
    uint8_t  _csPin;
    uint8_t  _gdo0Pin;
    uint8_t  _gdo2Pin;
    float    _mhzOsc;
    float    _currentFreq;
    SPISettings _spiSettings;

    void     spiSelect();
    void     spiDeselect();
    uint8_t  spiTransfer(uint8_t data);
};

#endif // CC1101_H
