/*
 * I2S audio output (44.1 kHz, 16-bit stereo): PCM5102 DAC on the T-Display S3, on-board ES8311 on the JC1060P470-V2.
 * Pins and codec setup come from the board component (board.h).
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#include <stddef.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#define AUDIO_SAMPLE_RATE 44100

esp_err_t audio_out_init(void);

/** Blocking write of interleaved 16-bit LE stereo samples */
esp_err_t audio_out_write(const void *data, size_t len, TickType_t timeout);

/**
 * Diagnostics: count rising edges on the I2S output pins (via PCNT, without disturbing the
 * outputs) and print the measured frequencies. DIN only toggles when non-silent audio is playing.
 */
void audio_out_check_pins(void);

/** Diagnostics: play a sine tone directly to I2S (blocking). Don't use while the player is playing. */
void audio_out_test_tone(int freq_hz, int duration_ms);
