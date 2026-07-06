#include "cc1101.h"
#include "custom_board.h"
#include "nrf_drv_spi.h"
#include "nrf_gpio.h"
#include "nrf_delay.h"
#include "app_error.h"

static const nrf_drv_spi_t m_spi = NRF_DRV_SPI_INSTANCE(SPI_INSTANCE);
static volatile bool m_spi_xfer_done = false;

static void spi_event_handler(nrf_drv_spi_evt_t const *p_event, void *p_context)
{
    m_spi_xfer_done = true;
}

static void spi_transfer(uint8_t *tx_buf, uint8_t tx_len, uint8_t *rx_buf, uint8_t rx_len)
{
    m_spi_xfer_done = false;
    nrf_drv_spi_xfer_desc_t xfer = {
        .p_tx_buffer = tx_buf, .tx_length = tx_len,
        .p_rx_buffer = rx_buf, .rx_length = rx_len
    };
    nrf_drv_spi_xfer(&m_spi, &xfer, NULL);
    while (!m_spi_xfer_done) {}
}

static void csn_low(void)  { nrf_gpio_pin_clear(PIN_SPI_CS); nrf_delay_us(1); }
static void csn_high(void) { nrf_gpio_pin_set(PIN_SPI_CS); }

void cc1101_init(void)
{
    nrf_gpio_cfg_output(PIN_CC1101_POWER);
    nrf_gpio_pin_clear(PIN_CC1101_POWER);
    nrf_gpio_cfg_output(PIN_SPI_CS);
    nrf_gpio_pin_set(PIN_SPI_CS);
    nrf_gpio_cfg_input(PIN_GDO0, NRF_GPIO_PIN_NOPULL);
    nrf_gpio_cfg_input(PIN_GDO2, NRF_GPIO_PIN_NOPULL);

    nrf_drv_spi_config_t spi_config = {
        .sck_pin = PIN_SPI_SCK, .mosi_pin = PIN_SPI_MOSI, .miso_pin = PIN_SPI_MISO,
        .ss_pin = NRF_DRV_SPI_PIN_NOT_USED, .irq_priority = APP_IRQ_PRIORITY_LOW,
        .orc = 0xFF, .frequency = NRF_DRV_SPI_FREQ_1M,
        .mode = NRF_DRV_SPI_MODE_0, .bit_order = NRF_DRV_SPI_BIT_ORDER_MSB_FIRST
    };
    APP_ERROR_CHECK(nrf_drv_spi_init(&m_spi, &spi_config, spi_event_handler, NULL));
}

void cc1101_power_on(void)
{
    nrf_gpio_pin_set(PIN_CC1101_POWER);
    nrf_delay_ms(10);
    cc1101_reset();
}

void cc1101_power_off(void) { nrf_gpio_pin_clear(PIN_CC1101_POWER); }
bool cc1101_is_powered(void) { return nrf_gpio_pin_read(PIN_CC1101_POWER) ? true : false; }

void cc1101_reset(void)
{
    csn_low(); nrf_delay_ms(1); csn_high(); nrf_delay_ms(1);
    cc1101_send_command(CC1101_SRES);
    nrf_delay_ms(10);
    uint32_t timeout = 1000;
    while (cc1101_get_marc_state() != 0x01 && timeout-- > 0) nrf_delay_us(100);
}

void cc1101_write_reg(uint8_t addr, uint8_t value)
{
    uint8_t tx[2] = {addr, value}, rx[2];
    csn_low(); spi_transfer(tx, 2, rx, 2); csn_high();
}

uint8_t cc1101_read_reg(uint8_t addr)
{
    uint8_t tx[2] = {addr | 0x80, 0}, rx[2];
    csn_low(); spi_transfer(tx, 2, rx, 2); csn_high();
    return rx[1];
}

void cc1101_write_burst(uint8_t addr, const uint8_t *data, uint8_t len)
{
    uint8_t hdr = addr | CC1101_WRITE_BURST, dummy;
    csn_low(); spi_transfer(&hdr, 1, &dummy, 0);
    spi_transfer((uint8_t*)data, len, &dummy, 0); csn_high();
}

void cc1101_read_burst(uint8_t addr, uint8_t *data, uint8_t len)
{
    uint8_t hdr = addr | CC1101_READ_BURST | 0x80;
    csn_low(); spi_transfer(&hdr, 1, NULL, 0);
    spi_transfer(NULL, 0, data, len); csn_high();
}

void cc1101_send_command(uint8_t cmd)
{
    uint8_t tx[1] = {cmd}, rx;
    csn_low(); spi_transfer(tx, 1, &rx, 1); csn_high();
}

void cc1101_flush_rx_fifo(void) { cc1101_send_command(CC1101_SFRX); }
void cc1101_flush_tx_fifo(void) { cc1101_send_command(CC1101_SFTX); }
void cc1101_idle(void)          { cc1101_send_command(CC1101_SIDLE); }
void cc1101_rx(void)            { cc1101_send_command(CC1101_SRX); }
void cc1101_tx(void)            { cc1101_send_command(CC1101_STX); }
void cc1101_power_down(void)    { cc1101_send_command(CC1101_SPWD); }

void cc1101_calibrate(void)
{
    cc1101_send_command(CC1101_SCAL);
    nrf_delay_ms(1);
}

uint8_t cc1101_get_marc_state(void) { return cc1101_read_reg(CC1101_MARCSTATE) & 0x1F; }

int8_t cc1101_get_rssi(void)
{
    int8_t r = (int8_t)cc1101_read_reg(CC1101_RSSI);
    return (r >= 128) ? (int8_t)((r - 256) / 2) - 74 : (r / 2) - 74;
}

uint8_t cc1101_get_rx_bytes(void) { return cc1101_read_reg(CC1101_RXBYTES) & 0x7F; }

void cc1101_set_frequency(float freq_mhz)
{
    uint32_t fw = (uint32_t)((freq_mhz * 65536.0f) / 26.0f);
    cc1101_write_reg(CC1101_FREQ2, (fw >> 16) & 0xFF);
    cc1101_write_reg(CC1101_FREQ1, (fw >> 8) & 0xFF);
    cc1101_write_reg(CC1101_FREQ0, fw & 0xFF);
    cc1101_calibrate();
}

void cc1101_set_modulation(uint8_t mod)
{
    uint8_t v = cc1101_read_reg(CC1101_MDMCFG2);
    cc1101_write_reg(CC1101_MDMCFG2, (v & 0x8F) | ((mod & 0x07) << 4));
}

void cc1101_set_data_rate(uint8_t rate) { cc1101_write_reg(CC1101_MDMCFG3, rate); }
void cc1101_set_deviation(uint8_t dev) { cc1101_write_reg(CC1101_DEVIATN, dev); }

void cc1101_set_sync_mode(uint8_t mode)
{
    uint8_t v = cc1101_read_reg(CC1101_MDMCFG2);
    cc1101_write_reg(CC1101_MDMCFG2, (v & 0xF8) | (mode & 0x07));
}

void cc1101_set_manchester(bool enable)
{
    uint8_t v = cc1101_read_reg(CC1101_MDMCFG2);
    cc1101_write_reg(CC1101_MDMCFG2, enable ? (v | 0x08) : (v & ~0x08));
}

void cc1101_set_rx_config(void)
{
    cc1101_write_reg(CC1101_MDMCFG2, 0x00);
    cc1101_write_reg(CC1101_PKTCTRL0, 0x02);
    cc1101_write_reg(CC1101_PKTCTRL1, 0x00);
    cc1101_write_reg(CC1101_MCSM1, 0x0C);
    cc1101_write_reg(CC1101_MCSM0, 0x08);
    cc1101_write_reg(CC1101_AGCCTRL2, 0x00);
    cc1101_write_reg(CC1101_AGCCTRL1, 0x00);
    cc1101_write_reg(CC1101_AGCCTRL0, 0x91);
}

void cc1101_set_tx_config(void)
{
    cc1101_write_reg(CC1101_MDMCFG2, 0x00);
    cc1101_write_reg(CC1101_PKTCTRL0, 0x00);
    cc1101_write_reg(CC1101_PKTCTRL1, 0x00);
    cc1101_write_reg(CC1101_MCSM1, 0x00);
}

void cc1101_send_packet(const uint8_t *data, uint8_t len)
{
    cc1101_idle();
    cc1101_flush_tx_fifo();
    cc1101_write_reg(CC1101_PKTLEN, len);
    cc1101_write_reg(CC1101_PKTCTRL0, 0x00);
    cc1101_write_burst(CC1101_TX_FIFO, data, len);
    cc1101_tx();
    uint32_t timeout = 10000;
    while (timeout-- > 0 && cc1101_get_marc_state() == 0x13) nrf_delay_us(100);
}

bool cc1101_receive_packet(uint8_t *data, uint8_t *len, int8_t *rssi)
{
    uint8_t rx = cc1101_get_rx_bytes();
    if (rx == 0) return false;
    if (rx > 64) rx = 64;
    if (rssi) *rssi = cc1101_get_rssi();
    cc1101_read_burst(CC1101_RX_FIFO, data, rx);
    *len = rx;
    return true;
}
