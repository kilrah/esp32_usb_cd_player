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

static const char *TAG = "s31_coreboard";

#define CODEC_VOLUME_DB       0.0f        /* the player's digital volume does the rest */

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
