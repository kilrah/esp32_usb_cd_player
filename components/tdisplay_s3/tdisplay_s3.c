/*
 * LilyGO T-Display S3: ST7789 (i80, 8-bit) + LVGL 9 initialization
 *
 * Based on https://github.com/hiruna/esp-idf-t-display-s3 (adapted to ESP-IDF 6: LVGL draw buffers in internal RAM,
 * dma_burst_size instead of the alignment fields; AW9364 backlight levels; no battery ADC).
 * SPDX-FileCopyrightText: © 2025 Hiruna Wijesinghe <hiruna.kawinda@gmail.com>
 * SPDX-License-Identifier: MIT
 */

#include "tdisplay_s3.h"
#include "esp_check.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_io_i80.h"
#include "esp_lvgl_port.h"

static const char *TAG = "tdisplay_s3";

#define LCD_PIXEL_CLOCK_HZ   (17 * 1000 * 1000) /* PSRAM bandwidth limits this */
#define LVGL_BUFFER_PIXELS   ((TDS3_LCD_H_RES * TDS3_LCD_V_RES) / 10 + TDS3_LCD_H_RES)

/*
 * Backlight: AW9364 LED driver, 16 levels set by pulses on its EN pin (GPIO38), as in LilyGO's setBrightness().
 * Rising edge from off = level 16 (max); each short low pulse (0.5..500 us low) steps one level down,
 * wrapping from 1 to 16. Low for > 2.5 ms switches it off.
 */
static portMUX_TYPE s_bl_lock = portMUX_INITIALIZER_UNLOCKED;
static int s_bl_level;

void tdisplay_s3_set_brightness(int level)
{
    level = level < 0 ? 0 : level > TDS3_BRIGHTNESS_MAX ? TDS3_BRIGHTNESS_MAX : level;
    if (level == s_bl_level) {
        return;
    }
    if (level == 0) {
        gpio_set_level(TDS3_PIN_LCD_BL, 0);
        esp_rom_delay_us(3000);
        s_bl_level = 0;
        return;
    }
    if (s_bl_level == 0) {
        gpio_set_level(TDS3_PIN_LCD_BL, 1);
        esp_rom_delay_us(30);
        s_bl_level = TDS3_BRIGHTNESS_MAX;
    }
    int pulses = (s_bl_level - level + TDS3_BRIGHTNESS_MAX) % TDS3_BRIGHTNESS_MAX;
    portENTER_CRITICAL(&s_bl_lock); /* a low phase stretched beyond 2.5 ms would switch the driver off */
    for (int i = 0; i < pulses; i++) {
        gpio_set_level(TDS3_PIN_LCD_BL, 0);
        esp_rom_delay_us(1);
        gpio_set_level(TDS3_PIN_LCD_BL, 1);
        esp_rom_delay_us(1);
    }
    portEXIT_CRITICAL(&s_bl_lock);
    s_bl_level = level;
}

int tdisplay_s3_get_brightness(void)
{
    return s_bl_level;
}

void tdisplay_s3_backlight(bool on)
{
    tdisplay_s3_set_brightness(on ? TDS3_BRIGHTNESS_MAX : 0);
}

esp_err_t tdisplay_s3_init(lv_display_t **disp)
{
    /* LCD power (needed when running from battery) and RD line idle high */
    const gpio_config_t out_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = BIT64(TDS3_PIN_LCD_PWR) | BIT64(TDS3_PIN_LCD_BL),
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out_cfg), TAG, "gpio");
    gpio_set_level(TDS3_PIN_LCD_PWR, 1);
    gpio_set_level(TDS3_PIN_LCD_BL, 0);
    const gpio_config_t rd_cfg = {
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pin_bit_mask = BIT64(TDS3_PIN_LCD_RD),
    };
    ESP_RETURN_ON_ERROR(gpio_config(&rd_cfg), TAG, "gpio");

    /* LVGL port (task on core 1, the USB/audio tasks are unpinned) */
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = 2,
        .task_stack = 6 * 1024,
        .task_affinity = 1,
        .task_max_sleep_ms = 50,
        .timer_period_ms = 5,
    };
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "lvgl port");

    /* i80 bus */
    esp_lcd_i80_bus_handle_t bus = NULL;
    const esp_lcd_i80_bus_config_t bus_cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .dc_gpio_num = TDS3_PIN_LCD_DC,
        .wr_gpio_num = TDS3_PIN_LCD_WR,
        .data_gpio_nums = {
            TDS3_PIN_LCD_D0, TDS3_PIN_LCD_D1, TDS3_PIN_LCD_D2, TDS3_PIN_LCD_D3,
            TDS3_PIN_LCD_D4, TDS3_PIN_LCD_D5, TDS3_PIN_LCD_D6, TDS3_PIN_LCD_D7,
        },
        .bus_width = 8,
        .max_transfer_bytes = LVGL_BUFFER_PIXELS * sizeof(uint16_t),
        .dma_burst_size = 64,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_i80_bus(&bus_cfg, &bus), TAG, "i80 bus");

    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_panel_io_i80_config_t io_cfg = {
        .cs_gpio_num = TDS3_PIN_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 20,
        .dc_levels = {
            .dc_idle_level = 0,
            .dc_cmd_level = 0,
            .dc_dummy_level = 0,
            .dc_data_level = 1,
        },
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i80(bus, &io_cfg, &io), TAG, "panel io");

    /* ST7789 panel: landscape, buttons on the left */
    esp_lcd_panel_handle_t panel = NULL;
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = TDS3_PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &panel_cfg, &panel), TAG, "st7789");
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, true);
    esp_lcd_panel_swap_xy(panel, true);
    esp_lcd_panel_mirror(panel, false, true);
    esp_lcd_panel_set_gap(panel, 0, 35);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "disp on");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = LVGL_BUFFER_PIXELS,
        .double_buffer = true,
        .hres = TDS3_LCD_H_RES,
        .vres = TDS3_LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = true,
            .mirror_x = false,
            .mirror_y = true,
        },
        .flags = {
            /* Draw buffers in internal DMA-capable RAM (2 x ~11.5 KB). With PSRAM buffers the
             * LCD DMA sometimes sent stale (not yet written back) cache content, which showed
             * up as leftover white areas behind updated labels. */
            .buff_dma = true,
            .buff_spiram = false,
            .swap_bytes = true,
        },
    };
    lv_display_t *d = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(d, ESP_FAIL, TAG, "lvgl add disp");
    if (disp) {
        *disp = d;
    }

    tdisplay_s3_backlight(true);
    ESP_LOGI(TAG, "Display initialized (%dx%d)", TDS3_LCD_H_RES, TDS3_LCD_V_RES);
    return ESP_OK;
}
