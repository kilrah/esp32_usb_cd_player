/*
 * Board interface used by the application (every board component provides this header)
 *
 * Espressif ESP32-S31-Function-CoreBoard-1 (ESP32-S31-WROOM-3: 16 MB flash, 16 MB PSRAM). No display: the player
 * is driven over serial and the web UI. The drive goes on the USB-A port (USB 2.0 OTG High-Speed).
 * Audio: on-board ES8311 + NS4150B speaker amplifier (mono, default) or an external PCM5102 on the J2 header.
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_eth_driver.h"

#define BOARD_NAME            "ESP32-S31-Function-CoreBoard-1"
#define BOARD_HAS_DISPLAY     0   /* headless: no UI, LVGL isn't built */
#define BOARD_HAS_TOUCH       0
#define BOARD_HAS_ETH         1
#define BOARD_LCD_H_RES       0
#define BOARD_LCD_V_RES       0

/* Ethernet: YT8531 gigabit PHY on RGMII (the IDF defaults for the ESP32-S31: ETH_ESP32_EMAC_DEFAULT_CONFIG) */
#define BOARD_PIN_ETH_MDC     5
#define BOARD_PIN_ETH_MDIO    6
#define BOARD_PIN_ETH_RST     7
#define BOARD_PIN_ETH_TXD0    8   /* TXD0-3 8-11 */
#define BOARD_PIN_ETH_TX_CTL  12
#define BOARD_PIN_ETH_TX_CLK  13
#define BOARD_PIN_ETH_RX_CLK  14
#define BOARD_PIN_ETH_RX_CTL  15
#define BOARD_PIN_ETH_RXD3    16  /* RXD3-0 16-19 (reversed) */

#define BOARD_PIN_I2C_SCL     50  /* ES8311 */
#define BOARD_PIN_I2C_SDA     51
#define BOARD_PIN_PA_EN       57  /* NS4150B speaker amplifier enable, active high */

#if CONFIG_S31CB_AUDIO_ES8311
/* On-board ES8311 codec (I2S slave, MCLK = 256 fs) */
#define BOARD_I2S_MCLK        52
#define BOARD_I2S_BCLK        53  /* I2S_SCLK */
#define BOARD_I2S_WS          55  /* I2S_LRCK */
#define BOARD_I2S_DOUT        56  /* I2S_DSDIN (ESP32 -> codec) */
#define BOARD_AUDIO_MONO      1   /* ES8311 has one DAC: the output mixes left and right */
#else
/* External PCM5102 module on the J2 header: VCC, GND, then pins 31/29/27/25 (menuconfig: Board > Audio output) */
#define BOARD_I2S_WS          25  /* PCM5102 LRCK */
#define BOARD_I2S_DOUT        23  /* PCM5102 DIN */
#define BOARD_I2S_BCLK        21  /* PCM5102 BCK */
#define BOARD_I2S_MCLK        35  /* PCM5102 SCK */
#define BOARD_AUDIO_MONO      0
#endif

/** Called once the I2S clocks run: configures the ES8311 and switches the speaker amplifier on
 *  (PCM5102 output: keeps the amplifier off) */
esp_err_t board_audio_init(void);

/** Ethernet MAC + PHY: installs the driver (not started) */
esp_err_t board_eth_init(esp_eth_handle_t *eth);
