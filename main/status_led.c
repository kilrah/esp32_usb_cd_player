/*
 * Status indication on the on-board WS2812B LED
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "led_strip.h"
#include "status_led.h"

static const char *TAG = "status_led";

#define BRIGHTNESS 24 /* 0..255, the LED is very bright at full power */

typedef struct {
    uint8_t r, g, b;
    uint16_t on_ms;  /*!< 0 = steady */
    uint16_t off_ms;
} pattern_t;

static const pattern_t s_patterns[] = {
    [STATUS_BOOT]         = {255, 255, 255, 0, 0},
    [STATUS_WAIT_DRIVE]   = {0, 0, 255, 150, 1350},
    [STATUS_NO_DISC]      = {255, 160, 0, 0, 0},
    [STATUS_DISC_LOADING] = {255, 160, 0, 150, 150},
    [STATUS_BUSY]         = {0, 200, 255, 0, 0},
    [STATUS_DISC_READY]   = {0, 255, 0, 0, 0},
    [STATUS_ERROR]        = {255, 0, 0, 100, 100},
};

static led_strip_handle_t s_strip;
static TaskHandle_t s_task;
static volatile status_t s_status = STATUS_BOOT;

static void led_show(const pattern_t *p, bool on)
{
    if (on) {
        led_strip_set_pixel(s_strip, 0, p->r * BRIGHTNESS / 255, p->g * BRIGHTNESS / 255, p->b * BRIGHTNESS / 255);
        led_strip_refresh(s_strip);
    } else {
        led_strip_clear(s_strip);
    }
}

static void led_task(void *arg)
{
    /* Boot indication: three white flashes, so a restart after flashing is obvious */
    for (int i = 0; i < 3; i++) {
        led_show(&s_patterns[STATUS_BOOT], true);
        vTaskDelay(pdMS_TO_TICKS(100));
        led_show(&s_patterns[STATUS_BOOT], false);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    while (true) {
        const pattern_t *p = &s_patterns[s_status];
        if (p->on_ms == 0) {
            led_show(p, true);
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY); /* wait for status change */
            continue;
        }
        led_show(p, true);
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(p->on_ms))) {
            continue;
        }
        led_show(p, false);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(p->off_ms));
    }
}

esp_err_t status_led_init(int gpio)
{
    const led_strip_config_t strip_config = {
        .strip_gpio_num = gpio,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip), TAG, "LED init failed");
    led_strip_clear(s_strip);
    ESP_RETURN_ON_FALSE(xTaskCreate(led_task, "status_led", 2560, NULL, 2, &s_task) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

void status_led_set(status_t status)
{
    if (status == s_status || !s_task) {
        s_status = status;
        return;
    }
    s_status = status;
    xTaskNotifyGive(s_task);
}
