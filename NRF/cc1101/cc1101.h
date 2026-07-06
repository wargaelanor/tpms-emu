#ifndef CC1101_H
#define CC1101_H

#include <stdint.h>
#include <stdbool.h>

// Command strobes
#define CC1101_SRES         0x30
#define CC1101_SCAL         0x33
#define CC1101_SRX          0x34
#define CC1101_STX          0x35
#define CC1101_SIDLE        0x36
#define CC1101_SFRX         0x3A
#define CC1101_SFTX         0x3B
#define CC1101_SPWD         0x39
#define CC1101_SNOP         0x3D

// Registers
#define CC1101_IOCFG2       0x00
#define CC1101_IOCFG0       0x02
#define CC1101_PKTLEN       0x06
#define CC1101_PKTCTRL1     0x07
#define CC1101_PKTCTRL0     0x08
#define CC1101_FREQ2        0x0D
#define CC1101_FREQ1        0x0E
#define CC1101_FREQ0        0x0F
#define CC1101_MDMCFG4      0x10
#define CC1101_MDMCFG3      0x11
#define CC1101_MDMCFG2      0x12
#define CC1101_DEVIATN      0x15
#define CC1101_MCSM1        0x17
#define CC1101_MCSM0        0x18
#define CC1101_AGCCTRL2     0x1B
#define CC1101_AGCCTRL1     0x1C
#define CC1101_AGCCTRL0     0x1D
#define CC1101_FSCAL3       0x23
#define CC1101_PATABLE      0x3E
#define CC1101_TX_FIFO      0x3F
#define CC1101_RX_FIFO      0x3F
#define CC1101_MARCSTATE    0x35
#define CC1101_RSSI         0x34
#define CC1101_RXBYTES      0x3B
#define CC1101_TXBYTES      0x3A
#define CC1101_PARTNUM      0x30
#define CC1101_VERSION      0x31

#define CC1101_WRITE_BURST  0x40
#define CC1101_READ_BURST   0xC0

void     cc1101_init(void);
void     cc1101_reset(void);
void     cc1101_power_on(void);
void     cc1101_power_off(void);
bool     cc1101_is_powered(void);

void     cc1101_write_reg(uint8_t addr, uint8_t value);
uint8_t  cc1101_read_reg(uint8_t addr);
void     cc1101_write_burst(uint8_t addr, const uint8_t *data, uint8_t len);
void     cc1101_read_burst(uint8_t addr, uint8_t *data, uint8_t len);

void     cc1101_send_command(uint8_t cmd);
void     cc1101_flush_rx_fifo(void);
void     cc1101_flush_tx_fifo(void);
void     cc1101_idle(void);
void     cc1101_rx(void);
void     cc1101_tx(void);
void     cc1101_calibrate(void);
void     cc1101_power_down(void);

uint8_t  cc1101_get_marc_state(void);
int8_t   cc1101_get_rssi(void);
uint8_t  cc1101_get_rx_bytes(void);

void     cc1101_set_frequency(float freq_mhz);
void     cc1101_set_modulation(uint8_t mod);
void     cc1101_set_data_rate(uint8_t rate);
void     cc1101_set_deviation(uint8_t dev);
void     cc1101_set_sync_mode(uint8_t mode);
void     cc1101_set_rx_config(void);
void     cc1101_set_tx_config(void);
void     cc1101_set_manchester(bool enable);

void     cc1101_send_packet(const uint8_t *data, uint8_t len);
bool     cc1101_receive_packet(uint8_t *data, uint8_t *len, int8_t *rssi);

#endif
