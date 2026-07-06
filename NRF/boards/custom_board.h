#ifndef CUSTOM_BOARD_H
#define CUSTOM_BOARD_H

// ============================================================================
// nRF52840 + CC1101 Pin Definitions
// ============================================================================

// CC1101 SPI pins
#define PIN_SPI_MOSI        13  // P0.13
#define PIN_SPI_MISO        15  // P0.15
#define PIN_SPI_SCK         14  // P0.14
#define PIN_SPI_CS          16  // P0.16

// CC1101 control pins
#define PIN_GDO0            11  // P0.11
#define PIN_GDO2            12  // P0.12
#define PIN_CC1101_POWER    10  // P0.10 (HIGH = ON)

// Status LED
#define PIN_LED_STATUS       7  // P0.07

// Button
#define PIN_BUTTON           8  // P0.08

// UART (debug)
#define PIN_UART_TX         20  // P0.20
#define PIN_UART_RX         19  // P0.19

// Battery ADC
#define PIN_BATTERY_ADC      4  // P0.04 (AIN2)

// SPI Instance
#define SPI_INSTANCE         0

// BLE
#define DEVICE_NAME          "TPMS-NRF"

// Battery
#define BATTERY_ADC_CHANNEL  2
#define BATTERY_VOLTAGE_DIV  2

#endif
