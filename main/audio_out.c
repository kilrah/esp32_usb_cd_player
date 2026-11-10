/*
 * I2S audio output (44.1 kHz, 16-bit stereo), see audio_out.h
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "audio_out.h"
#include "esp_check.h"
#include "esp_rom_sys.h"
#include "driver/i2s_std.h"
#include "driver/pulse_cnt.h"
#include "board.h"

#define AUDIO_PIN_MCLK BOARD_I2S_MCLK
#define AUDIO_PIN_BCLK BOARD_I2S_BCLK
#define AUDIO_PIN_WS   BOARD_I2S_WS
#define AUDIO_PIN_DOUT BOARD_I2S_DOUT

static const char *TAG = "audio_out";
static i2s_chan_handle_t s_tx;

esp_err_t audio_out_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 512;   /* 8 x 512 frames = ~93 ms of DMA buffering */
    chan_cfg.auto_clear = true;     /* send silence on underrun / pause */
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "new channel");

    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE), /* MCLK = 256 fs */
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = AUDIO_PIN_MCLK,
            .bclk = AUDIO_PIN_BCLK,
            .ws = AUDIO_PIN_WS,
            .dout = AUDIO_PIN_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {0},
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "std mode");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "enable");
    ESP_LOGI(TAG, "I2S out: %d Hz, 16-bit stereo, MCLK %d BCK %d WS %d DOUT %d", AUDIO_SAMPLE_RATE,
             AUDIO_PIN_MCLK, AUDIO_PIN_BCLK, AUDIO_PIN_WS, AUDIO_PIN_DOUT);
    /* Codec / amplifier setup needs MCLK running. Not fatal: the player (and the I2S pins) keep working. */
    esp_err_t err = board_audio_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Audio codec init failed: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

#if BOARD_AUDIO_MONO
/* One DAC (it plays the left slot): mix both channels into both slots, in chunks */
esp_err_t audio_out_write(const void *data, size_t len, TickType_t timeout)
{
    static int16_t mix[1024]; /* only the player task (or the test tone, never at the same time) writes */
    const int16_t *in = data;
    size_t frames = len / 4;
    while (frames) {
        size_t n = frames < sizeof(mix) / 4 ? frames : sizeof(mix) / 4;
        for (size_t i = 0; i < n; i++) {
            int16_t m = (int16_t)(((int32_t)in[2 * i] + in[2 * i + 1]) / 2);
            mix[2 * i] = mix[2 * i + 1] = m;
        }
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx, mix, n * 4, &written, pdTICKS_TO_MS(timeout));
        if (err != ESP_OK) {
            return err;
        }
        in += 2 * n;
        frames -= n;
    }
    return ESP_OK;
}
#else
esp_err_t audio_out_write(const void *data, size_t len, TickType_t timeout)
{
    size_t written = 0;
    return i2s_channel_write(s_tx, data, len, &written, pdTICKS_TO_MS(timeout));
}
#endif

/* ------------------------------------------------------------------------ */
/* Diagnostics                                                              */
/* ------------------------------------------------------------------------ */

static int count_edges(int gpio, int window_us)
{
    pcnt_unit_handle_t unit = NULL;
    pcnt_channel_handle_t chan = NULL;
    const pcnt_unit_config_t unit_cfg = {.low_limit = -32768, .high_limit = 32767};
    if (pcnt_new_unit(&unit_cfg, &unit) != ESP_OK) {
        return -1;
    }
    const pcnt_chan_config_t chan_cfg = {.edge_gpio_num = gpio, .level_gpio_num = -1};
    int count = -1;
    if (pcnt_new_channel(unit, &chan_cfg, &chan) == ESP_OK) {
        pcnt_channel_set_edge_action(chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD);
        pcnt_unit_enable(unit);
        pcnt_unit_clear_count(unit);
        pcnt_unit_start(unit);
        esp_rom_delay_us(window_us);
        pcnt_unit_stop(unit);
        pcnt_unit_get_count(unit, &count);
        pcnt_unit_disable(unit);
        pcnt_del_channel(chan);
    }
    pcnt_del_unit(unit);
    return count;
}

void audio_out_check_pins(void)
{
    static const struct {
        const char *name;
        int gpio;
        int expect_hz;
    } pins[] = {
        {"MCLK (SCK)", AUDIO_PIN_MCLK, AUDIO_SAMPLE_RATE * 256},
        {"BCK", AUDIO_PIN_BCLK, AUDIO_SAMPLE_RATE * 32},
        {"LCK (WS)", AUDIO_PIN_WS, AUDIO_SAMPLE_RATE},
        {"DIN", AUDIO_PIN_DOUT, -1},
    };
    const int window_us = 2000; /* MCLK: ~22600 edges, fits the 16-bit counter */
    for (int i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        int n = count_edges(pins[i].gpio, window_us);
        int hz = n < 0 ? -1 : (int)((int64_t)n * 1000000 / window_us);
        if (pins[i].expect_hz > 0) {
            printf("AUDIO %-10s GPIO%-2d %8d Hz (expected %d)%s\n", pins[i].name, pins[i].gpio, hz, pins[i].expect_hz,
                   hz < pins[i].expect_hz * 9 / 10 || hz > pins[i].expect_hz * 11 / 10 ? "  <-- WRONG" : "");
        } else {
            printf("AUDIO %-10s GPIO%-2d %8d edges/s (0 = silence)\n", pins[i].name, pins[i].gpio, hz);
        }
    }
}

void audio_out_test_tone(int freq_hz, int duration_ms)
{
    const int frames = 441; /* 10 ms */
    int16_t *buf = malloc(frames * 2 * sizeof(int16_t));
    if (!buf) {
        return;
    }
    float phase = 0, step = 2.0f * (float)M_PI * freq_hz / AUDIO_SAMPLE_RATE;
    for (int t = 0; t < duration_ms; t += 10) {
        for (int i = 0; i < frames; i++) {
            int16_t v = (int16_t)(sinf(phase) * 8000); /* about -12 dBFS */
            buf[2 * i] = v;
            buf[2 * i + 1] = v;
            phase += step;
            if (phase > 2.0f * (float)M_PI) {
                phase -= 2.0f * (float)M_PI;
            }
        }
        audio_out_write(buf, frames * 2 * sizeof(int16_t), pdMS_TO_TICKS(1000));
    }
    free(buf);
}
