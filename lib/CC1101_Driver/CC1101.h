#ifndef CC1101_H
#define CC1101_H

/*
 * CC1101 Driver for nRF52840 + CC1101 TPMS project
 */

#include <Arduino.h>
#include <SPI.h>

// Configuration Registers
#define CC1101_IOCFG2       0x00
#define CC1101_IOCFG1       0x01
#define CC1101_IOCFG0       0x02
#define CC1101_FIFOTHR      0x03
#define CC1101_SYNC1        0x04
#define CC1101_SYNC0        0x05
#define CC1101_PKTLEN       0x06
#define CC1101_PKTCTRL1     0x07
#define CC1101_PKTCTRL0     0x08
#define CC1101_ADDR         0x09
#define CC1101_CHANNR       0x0A
#define CC1101_FSCTRL1      0x0B
#define CC1101_FSCTRL0      0x0C
#define CC1101_FREQ2        0x0D
#define CC1101_FREQ1        0x0E
#define CC1101_FREQ0        0x0F
#define CC1101_MDMCFG4      0x10
#define CC1101_MDMCFG3      0x11
#define CC1101_MDMCFG2      0x12
#define CC1101_MDMCFG1      0x13
#define CC1101_MDMCFG0      0x14
#define CC1101_DEVIATN      0x15
#define CC1101_MCSM2        0x16
#define CC1101_MCSM1        0x17
#define CC1101_MCSM0        0x18
#define CC1101_FOCCFG       0x19
#define CC1101_BSCFG        0x1A
#define CC1101_AGCCTRL2     0x1B
#define CC1101_AGCCTRL1     0x1C
#define CC1101_AGCCTRL0     0x1D
#define CC1101_WOREVT1      0x1E
#define CC1101_WOREVT0      0x1F
#define CC1101_WORCTRL      0x20
#define CC1101_FREND1       0x21
#define CC1101_FREND0       0x22
#define CC1101_FSCAL3       0x23
#define CC1101_FSCAL2       0x24
#define CC1101_FSCAL1       0x25
#define CC1101_FSCAL0       0x26
#define CC1101_RCCTRL1      0x27
#define CC1101_RCCTRL0      0x28
#define CC1101_FSTEST       0x29
#define CC1101_PTEST        0x2A
#define CC1101_AGCTEST      0x2B
#define CC1101_TEST2        0x2C
#define CC1101_TEST1        0x2D
#define CC1101_TEST0        0x2E

// Status Registers
#define CC1101_PARTNUM      0x30
#define CC1101_VERSION      0x31
#define CC1101_FREQEST      0x32
#define CC1101_LQI          0x33
#define CC1101_RSSI         0x34
#define CC1101_MARCSTATE    0x35
#define CC1101_WORTIME1     0x36
#define CC1101_WORTIME0     0x37
#define CC1101_PKTSTATUS    0x38
#define CC1101_VCO_VC_DAC   0x39
#define CC1101_TXBYTES      0x3A
#define CC1101_RXBYTES      0x3B
#define CC1101_RCCTRL1_STATUS 0x3C
#define CC1101_RCCTRL0_STATUS 0x3D

// Strobe Commands
#define CC1101_SRES         0x30
#define CC1101_SFSTXON      0x31
#define CC1101_SXOFF        0x32
#define CC1101_SCAL         0x33
#define CC1101_SRX          0x34
#define CC1101_STX          0x35
#define CC1101_SIDLE        0x36
#define CC1101_SWOR         0x38
#define CC1101_SPWD         0x39
#define CC1101_SFRX         0x3A
#define CC1101_SFTX         0x3B
#define CC1101_SWORRST      0x3C
#define CC1101_SNOP         0x3D

// FIFO addresses
#define CC1101_TXFIFO       0x3F
#define CC1101_RXFIFO       0x3F
#define CC1101_TX_FIFO      CC1101_TXFIFO
#define CC1101_RX_FIFO      CC1101_RXFIFO

// MARC States
#define CC1101_MARCSTATE_SLEEP            0x00
#define CC1101_MARCSTATE_IDLE             0x01
#define CC1101_MARCSTATE_XOFF             0x02
#define CC1101_MARCSTATE_VCOON_MC         0x03
#define CC1101_MARCSTATE_REGON_MC         0x04
#define CC1101_MARCSTATE_MANCAL           0x05
#define CC1101_MARCSTATE_VCOON            0x06
#define CC1101_MARCSTATE_REGON            0x07
#define CC1101_MARCSTATE_STARTCAL         0x08
#define CC1101_MARCSTATE_BWBOOST          0x09
#define CC1101_MARCSTATE_FS_LOCK          0x0A
#define CC1101_MARCSTATE_IFADCON          0x0B
#define CC1101_MARCSTATE_ENDCAL           0x0C
#define CC1101_MARCSTATE_RX               0x0D
#define CC1101_MARCSTATE_RX_END           0x0E
#define CC1101_MARCSTATE_RX_RST           0x0F
#define CC1101_MARCSTATE_TXRX_SWITCH      0x10
#define CC1101_MARCSTATE_RXFIFO_OVERFLOW  0x11
#define CC1101_MARCSTATE_FSTXON           0x12
#define CC1101_MARCSTATE_TX               0x13
#define CC1101_MARCSTATE_TX_END           0x14
#define CC1101_MARCSTATE_RXTX_SWITCH      0x15
#define CC1101_MARCSTATE_TXFIFO_UNDERFLOW 0x16

class CC1101 {
public:
    CC1101(uint8_t csPin, uint8_t gdo0Pin, uint8_t gdo2Pin);

    void    init();
    void    reset();
    void    wakeUp();

    void    setFreq(uint32_t freqHz);
    bool    setFreq(float frequency);
    void    setFreqConfig(uint8_t freq2, uint8_t freq1, uint8_t freq0);
    void    setFreqConfig(float frequency);
    bool    setDRate(uint32_t baud);
    bool    setModulation(uint8_t mod);
    bool    setDeviation(float khz);
    void    setPA(int8_t index);
    void    setPApower(int8_t dBm);

    void    setSyncWord(uint8_t syncH, uint8_t syncL);
    void    setSyncMode(uint8_t mode);
    void    setPktLength(uint8_t length);
    void    setCrc(bool enable);
    void    setWhiteData(bool enable);
    void    setManc(uint8_t enable);
    void    setPqt(uint8_t pqt);
    void    setMHZOsc(float mhz);
    void    setAddr(uint8_t addr);
    void    setChannr(uint8_t ch);

    bool    sendData(const uint8_t *data, uint8_t length);
    void    setTxState();
    void    setRxState();
    void    setIdleState();

    bool    receiveData(uint8_t *data, uint8_t *length);
    void    setRxConfig();

    int8_t  getRssi();
    uint8_t getLqi();
    uint8_t getMarcState();
    uint8_t getRxBytes();
    uint8_t getTxBytes();
    uint8_t getChipState();
    uint8_t getChipVersion();
    uint8_t getPartNumber();

    uint8_t readReg(uint8_t reg);
    void    writeReg(uint8_t reg, uint8_t value);
    uint8_t readStatusReg(uint8_t reg);
    void    writeBurstReg(uint8_t reg, const uint8_t *data, uint8_t len);
    void    readBurstReg(uint8_t reg, uint8_t *data, uint8_t len);
    void    sendCommand(uint8_t cmd);

    void    flushRxFifo();
    void    flushTxFifo();
    void    printRegs();

private:
    uint8_t _csPin;
    uint8_t _gdo0Pin;
    uint8_t _gdo2Pin;
    float   _mhzOsc;
    float   _currentFreq;
    SPISettings _spiSettings;

    void    spiSelect();
    void    spiDeselect();
    void    setSpiSettings();
    uint8_t spiTransfer(uint8_t data);
};

#endif // CC1101_H
