/*
 * Espressif ESP32-S31-Function-CoreBoard-1: ES8311 codec + NS4150B speaker amplifier (no display)
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include "board.h"
#include "esp_check.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "esp_eth.h"
#include "esp_eth_phy_yt8531.h"

static const char *TAG = "s31_coreboard";

#define CODEC_VOLUME_DB       0.0f        /* the player's digital volume does the rest */

/* ------------------------------------------------------------------------ */
/* Ethernet: EMAC (RGMII) + YT8531                                          */
/* ------------------------------------------------------------------------ */

esp_err_t board_eth_init(esp_eth_handle_t *eth)
{
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_cfg.interface = EMAC_DATA_INTERFACE_RGMII;
    emac_cfg.smi_gpio.mdc_num = BOARD_PIN_ETH_MDC;
    emac_cfg.smi_gpio.mdio_num = BOARD_PIN_ETH_MDIO;
    emac_cfg.clock_config.rgmii.clock_tx_gpio = BOARD_PIN_ETH_TX_CLK;
    emac_cfg.clock_config.rgmii.clock_rx_gpio = BOARD_PIN_ETH_RX_CLK;
    emac_cfg.clock_config.rgmii.clock_phy_ref_gpio = -1; /* the PHY has its own crystal */
    emac_cfg.emac_dataif_gpio.rgmii = (eth_mac_rgmii_gpio_config_t){
        .tx_ctl_num = BOARD_PIN_ETH_TX_CTL,
        .txd0_num = BOARD_PIN_ETH_TXD0,
        .txd1_num = BOARD_PIN_ETH_TXD0 + 1,
        .txd2_num = BOARD_PIN_ETH_TXD0 + 2,
        .txd3_num = BOARD_PIN_ETH_TXD0 + 3,
        .rx_ctl_num = BOARD_PIN_ETH_RX_CTL,
        .rxd0_num = BOARD_PIN_ETH_RXD3 + 3,
        .rxd1_num = BOARD_PIN_ETH_RXD3 + 2,
        .rxd2_num = BOARD_PIN_ETH_RXD3 + 1,
        .rxd3_num = BOARD_PIN_ETH_RXD3,
    };
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);

    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = ESP_ETH_PHY_ADDR_AUTO;
    phy_cfg.reset_gpio_num = BOARD_PIN_ETH_RST;
    esp_eth_phy_t *phy = esp_eth_phy_new_yt8531(&phy_cfg);

    esp_err_t err = ESP_FAIL;
    if (mac && phy) {
        esp_eth_config_t cfg = ETH_DEFAULT_CONFIG(mac, phy);
        err = esp_eth_driver_install(&cfg, eth);
    }
    if (err != ESP_OK) {
        if (phy) {
            phy->del(phy);
        }
        if (mac) {
            mac->del(mac);
        }
        ESP_LOGE(TAG, "Ethernet init failed: %s", esp_err_to_name(err));
        return err;
    }
    /* RGMII clock delays of this board (Espressif's esp-board-manager definition) */
    yt8531_rgmii_clk_delay_config_t dly = {.rxc_dly_en = true, .rx_delay_sel = 0, .tx_delay_sel = 13, .tx_delay_sel_fe = 13};
    err = esp_eth_ioctl(*eth, YT8531_ETH_CMD_S_RGMII_CLK_DELAY, &dly);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "YT8531 clock delays: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Audio: ES8311 (I2S slave) + NS4150B amplifier                            */
/* ------------------------------------------------------------------------ */

esp_err_t board_audio_init(void)
{
#if !CONFIG_S31CB_AUDIO_ES8311
    /* PCM5102 on the header: nothing to set up. Keep the speaker amplifier off (no input, it would only hiss). */
    const gpio_config_t pa = {.mode = GPIO_MODE_OUTPUT, .pin_bit_mask = BIT64(BOARD_PIN_PA_EN)};
    gpio_config(&pa);
    gpio_set_level(BOARD_PIN_PA_EN, 0);
    return ESP_OK;
#else
    i2c_master_bus_handle_t bus;
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1, /* any free port */
        .sda_io_num = BOARD_PIN_I2C_SDA,
        .scl_io_num = BOARD_PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), TAG, "i2c bus");
    audio_codec_i2c_cfg_t i2c_cfg = {.addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = bus};
    const audio_codec_ctrl_if_t *ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(ctrl && gpio, ESP_ERR_NO_MEM, TAG, "codec if");
    es8311_codec_cfg_t cfg = {
        .ctrl_if = ctrl,
        .gpio_if = gpio,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = BOARD_PIN_PA_EN,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .mclk_div = 256,
        .hw_gain = {.pa_voltage = 5.0f, .codec_dac_voltage = 3.3f},
    };
    const audio_codec_if_t *codec = es8311_codec_new(&cfg); /* also opens it */
    ESP_RETURN_ON_FALSE(codec, ESP_FAIL, TAG, "ES8311 not found");
    esp_codec_dev_sample_info_t fs = {.bits_per_sample = 16, .channel = 2, .sample_rate = 44100, .mclk_multiple = 256};
    ESP_RETURN_ON_FALSE(codec->set_fs(codec, &fs) == 0, ESP_FAIL, TAG, "ES8311 format");
    ESP_RETURN_ON_FALSE(codec->enable(codec, true) == 0, ESP_FAIL, TAG, "ES8311 enable"); /* also the amplifier */
    codec->set_vol(codec, CODEC_VOLUME_DB);
    ESP_LOGI(TAG, "ES8311 + speaker amplifier on");
    return ESP_OK;
#endif
}
