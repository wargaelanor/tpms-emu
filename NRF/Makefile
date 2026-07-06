# TPMS NRF52840 — Makefile (nRF5 SDK + GCC, no Arduino)

TARGET_NAME = tpms_nrf52840
BOARD = custom_board
SDK_ROOT ?= $(HOME)/nRF5_SDK_17.1.0
PROJ_DIR = .
SOFTDEVICE = s140
GNU_INSTALL_ROOT ?= /usr/bin/
GNU_PREFIX = arm-none-eabi
OUTPUT_DIRECTORY = _build

SRC_FILES = \
    $(PROJ_DIR)/main.c \
    $(PROJ_DIR)/cc1101/cc1101.c \
    $(PROJ_DIR)/ble/ble_tpms.c \
    $(SDK_ROOT)/modules/nrfx/soc/nrfx_atomic.c \
    $(SDK_ROOT)/modules/nrfx/drivers/src/nrfx_saadc.c \
    $(SDK_ROOT)/modules/nrfx/drivers/src/nrfx_spi.c \
    $(SDK_ROOT)/modules/nrfx/drivers/src/nrfx_spim.c \
    $(SDK_ROOT)/modules/nrfx/drivers/src/nrfx_gpiote.c \
    $(SDK_ROOT)/modules/nrfx/mdk/system_nrf52840.c \
    $(SDK_ROOT)/components/ble/common/ble_advdata.c \
    $(SDK_ROOT)/components/ble/common/ble_advertising.c \
    $(SDK_ROOT)/components/ble/common/ble_conn_params.c \
    $(SDK_ROOT)/components/ble/common/ble_conn_state.c \
    $(SDK_ROOT)/components/ble/common/ble_srv_common.c \
    $(SDK_ROOT)/components/ble/nrf_ble_gatt/nrf_ble_gatt.c \
    $(SDK_ROOT)/components/ble/nrf_ble_qwr/nrf_ble_qwr.c \
    $(SDK_ROOT)/components/libraries/log/src/nrf_log_backend_rtt.c \
    $(SDK_ROOT)/components/libraries/log/src/nrf_log_backend_serial.c \
    $(SDK_ROOT)/components/libraries/log/src/nrf_log_default_backends.c \
    $(SDK_ROOT)/components/libraries/log/src/nrf_log_frontend.c \
    $(SDK_ROOT)/components/libraries/log/src/nrf_log_str_formatter.c \
    $(SDK_ROOT)/components/libraries/util/app_error.c \
    $(SDK_ROOT)/components/libraries/util/app_error_handler_gcc.c \
    $(SDK_ROOT)/components/libraries/util/app_error_weak.c \
    $(SDK_ROOT)/components/libraries/util/app_scheduler.c \
    $(SDK_ROOT)/components/libraries/util/app_timer.c \
    $(SDK_ROOT)/components/libraries/util/app_util_platform.c \
    $(SDK_ROOT)/components/libraries/util/nrf_assert.c \
    $(SDK_ROOT)/components/libraries/atomic/nrf_atomic.c \
    $(SDK_ROOT)/components/libraries/balloc/nrf_balloc.c \
    $(SDK_ROOT)/components/libraries/memobj/nrf_memobj.c \
    $(SDK_ROOT)/components/libraries/ringbuf/nrf_ringbuf.c \
    $(SDK_ROOT)/components/libraries/strerror/nrf_strerror.c \
    $(SDK_ROOT)/integration/nrfx/legacy/nrf_drv_spi.c \
    $(SDK_ROOT)/integration/nrfx/legacy/nrf_drv_saadc.c \
    $(SDK_ROOT)/external/segger_rtt/SEGGER_RTT.c \
    $(SDK_ROOT)/external/segger_rtt/SEGGER_RTT_Syscalls_GCC.c \
    $(SDK_ROOT)/external/segger_rtt/SEGGER_RTT_printf.c \
    $(SDK_ROOT)/components/softdevice/common/nrf_sdh.c \
    $(SDK_ROOT)/components/softdevice/common/nrf_sdh_ble.c \
    $(SDK_ROOT)/components/softdevice/common/nrf_sdh_soc.c

INC_FOLDERS = \
    $(PROJ_DIR) $(PROJ_DIR)/cc1101 $(PROJ_DIR)/ble $(PROJ_DIR)/boards \
    $(SDK_ROOT)/components/ble/nrf_ble_gatt \
    $(SDK_ROOT)/components/ble/nrf_ble_qwr \
    $(SDK_ROOT)/components/ble/common \
    $(SDK_ROOT)/components/libraries/log \
    $(SDK_ROOT)/components/libraries/log/src \
    $(SDK_ROOT)/components/libraries/util \
    $(SDK_ROOT)/components/libraries/atomic \
    $(SDK_ROOT)/components/libraries/balloc \
    $(SDK_ROOT)/components/libraries/memobj \
    $(SDK_ROOT)/components/libraries/ringbuf \
    $(SDK_ROOT)/components/libraries/strerror \
    $(SDK_ROOT)/components/softdevice/common \
    $(SDK_ROOT)/components/softdevice/s140/headers \
    $(SDK_ROOT)/components/softdevice/s140/headers/nrf52 \
    $(SDK_ROOT)/integration/nrfx \
    $(SDK_ROOT)/integration/nrfx/legacy \
    $(SDK_ROOT)/modules/nrfx \
    $(SDK_ROOT)/modules/nrfx/drivers/include \
    $(SDK_ROOT)/modules/nrfx/hal \
    $(SDK_ROOT)/modules/nrfx/mdk \
    $(SDK_ROOT)/external/segger_rtt

OPT = -O3 -g3
CFLAGS = $(OPT) -DAPP_TIMER_V2 -DAPP_TIMER_V2_RTC1_ENABLED -DBOARD_$(BOARD) \
    -DCONFIG_GPIO_AS_PINRESET -DFLOAT_ABI_HARD -DNRF52840_XXAA \
    -DNRF_SD_BLE_API_VERSION=7 -DS140 -DSOFTDEVICE_PRESENT -DSWI_DISABLE0 \
    -mcpu=cortex-m4 -mthumb -mabi=aapcs -mfloat-abi=hard -mfpu=fpv4-sp-d16 \
    -ffunction-sections -fdata-sections -fno-strict-aliasing -fno-builtin -fshort-enums -Wall
LDFLAGS = $(OPT) -mcpu=cortex-m4 -mthumb -mabi=aapcs -mfloat-abi=hard -mfpu=fpv4-sp-d16 \
    -Wl,--gc-sections --specs=nano.specs -T$(PROJ_DIR)/linker.ld

.PHONY: all clean flash

all: $(OUTPUT_DIRECTORY)/$(TARGET_NAME).hex

$(OUTPUT_DIRECTORY)/$(TARGET_NAME).hex: $(OUTPUT_DIRECTORY)/$(TARGET_NAME).elf
	$(GNU_INSTALL_ROOT)$(GNU_PREFIX)-objcopy -O ihex $< $@

$(OUTPUT_DIRECTORY)/$(TARGET_NAME).elf: $(SRC_FILES)
	@mkdir -p $(OUTPUT_DIRECTORY)
	$(GNU_INSTALL_ROOT)$(GNU_PREFIX)-gcc $(CFLAGS) $(addprefix -I,$(INC_FOLDERS)) $^ -o $@ $(LDFLAGS)

flash: $(OUTPUT_DIRECTORY)/$(TARGET_NAME).hex
	nrfjprog --program $< --sectorerase --verify --reset

clean:
	rm -rf $(OUTPUT_DIRECTORY)
