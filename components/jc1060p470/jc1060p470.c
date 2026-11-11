/*
 * Guition JC1060P470 (ESP32-P4): display (MIPI-DSI, JD9165) + LVGL 9, GT911 touch, backlight, ES8311 codec
 *
 * Panel init sequence and timings from the board vendor's demo (as used in the p4_edge_enhancer project).
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include "board.h"
#include "esp_check.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_jd9165.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lvgl_port.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "esp_attr.h"
#include "esp_eth.h"
#include <inttypes.h>
#include "src/display/lv_display_private.h" /* flush_done_cb() */

static const char *TAG = "jc1060p470";

#define MIPI_PHY_LDO_CHAN     3
#define MIPI_PHY_LDO_MV       2500
#define LVGL_BUFFER_LINES     60          /* partial rendering: 2 x 1024 x 60 px RGB565 in PSRAM (2 x 120 KB) */
#define BL_LEDC_TIMER         LEDC_TIMER_1
#define BL_LEDC_CHANNEL       LEDC_CHANNEL_0
#define BL_DUTY_BITS          LEDC_TIMER_10_BIT
#define CODEC_VOLUME_DB       0.0f        /* the player's digital volume does the rest */

/* JD9165 init sequence for this panel (vendor) */
static const jd9165_lcd_init_cmd_t lcd_cmd[] = {
    {0x30, (uint8_t[]){0x00}, 1, 0},
    {0xF7, (uint8_t[]){0x49,0x61,0x02,0x00}, 4, 0},
    {0x30, (uint8_t[]){0x01}, 1, 0},
    {0x04, (uint8_t[]){0x0C}, 1, 0},
    {0x05, (uint8_t[]){0x08}, 1, 0},
    {0x0B, (uint8_t[]){0x11}, 1, 0},
    {0x20, (uint8_t[]){0x04}, 1, 0},
    {0x1F, (uint8_t[]){0x05}, 1, 0},
    {0x23, (uint8_t[]){0x38}, 1, 0},
    {0x28, (uint8_t[]){0x18}, 1, 0},
    {0x29, (uint8_t[]){0x29}, 1, 0},
    {0x2A, (uint8_t[]){0x01}, 1, 0},
    {0x2B, (uint8_t[]){0x29}, 1, 0},
    {0x2C, (uint8_t[]){0x01}, 1, 0},
    {0x30, (uint8_t[]){0x02}, 1, 0},
    {0x00, (uint8_t[]){0x05}, 1, 0},
    {0x01, (uint8_t[]){0x22}, 1, 0},
    {0x02, (uint8_t[]){0x08}, 1, 0},
    {0x03, (uint8_t[]){0x12}, 1, 0},
    {0x04, (uint8_t[]){0x16}, 1, 0},
    {0x05, (uint8_t[]){0x64}, 1, 0},
    {0x06, (uint8_t[]){0x00}, 1, 0},
    {0x07, (uint8_t[]){0x00}, 1, 0},
    {0x08, (uint8_t[]){0x78}, 1, 0},
    {0x09, (uint8_t[]){0x00}, 1, 0},
    {0x0A, (uint8_t[]){0x04}, 1, 0},
    {0x0B, (uint8_t[]){0x16,0x17,0x0B,0x0D,0x0D,0x0D,0x11,0x10,0x07,0x07,0x09}, 11, 0},
    {0x0C, (uint8_t[]){0x09,0x1E,0x1E,0x1C,0x1C,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D}, 11, 0},
    {0x0D, (uint8_t[]){0x0A,0x05,0x0B,0x0D,0x0D,0x0D,0x11,0x10,0x06,0x06,0x08}, 11, 0},
    {0x0E, (uint8_t[]){0x08,0x1F,0x1F,0x1D,0x1D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D}, 11, 0},
    {0x0F, (uint8_t[]){0x0A,0x05,0x0D,0x0B,0x0D,0x0D,0x11,0x10,0x1D,0x1D,0x1F}, 11, 0},
    {0x10, (uint8_t[]){0x1F,0x08,0x08,0x06,0x06,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D}, 11, 0},
    {0x11, (uint8_t[]){0x16,0x17,0x0D,0x0B,0x0D,0x0D,0x11,0x10,0x1C,0x1C,0x1E}, 11, 0},
    {0x12, (uint8_t[]){0x1E,0x09,0x09,0x07,0x07,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D}, 11, 0},
    {0x13, (uint8_t[]){0x00,0x00,0x00,0x00}, 4, 0},
    {0x14, (uint8_t[]){0x00,0x00,0x41,0x41}, 4, 0},
    {0x15, (uint8_t[]){0x00,0x00,0x00,0x00}, 4, 0},
    {0x17, (uint8_t[]){0x00}, 1, 0},
    {0x18, (uint8_t[]){0x85}, 1, 0},
    {0x19, (uint8_t[]){0x06,0x09}, 2, 0},
    {0x1A, (uint8_t[]){0x05,0x08}, 2, 0},
    {0x1B, (uint8_t[]){0x0A,0x04}, 2, 0},
    {0x26, (uint8_t[]){0x00}, 1, 0},
    {0x27, (uint8_t[]){0x00}, 1, 0},
    {0x30, (uint8_t[]){0x06}, 1, 0},
    {0x12, (uint8_t[]){0x3F,0x25,0x27,0x35,0x1D,0x1B,0x1B,0x1A,0x18,0x0A,0x2A,0x21,0x19,0x30}, 14, 0},
    {0x13, (uint8_t[]){0x3F,0x26,0x27,0x35,0x1E,0x1C,0x1C,0x1A,0x18,0x0B,0x2A,0x21,0x19,0x30}, 14, 0},
    {0x30, (uint8_t[]){0x0A}, 1, 0},
    {0x02, (uint8_t[]){0x4F}, 1, 0},
    {0x0B, (uint8_t[]){0x40}, 1, 0},
    {0x30, (uint8_t[]){0x0D}, 1, 0},
    {0x0D, (uint8_t[]){0x04}, 1, 0},
    {0x10, (uint8_t[]){0x05}, 1, 0},
    {0x11, (uint8_t[]){0x0C}, 1, 0},
    {0x12, (uint8_t[]){0x05}, 1, 0},
    {0x13, (uint8_t[]){0x0C}, 1, 0},
    {0x30, (uint8_t[]){0x00}, 1, 0},

    {0x3A, (uint8_t[]){0x55}, 1, 0},    // 16bit 565
    {0x11, (uint8_t[]){0x00}, 1, 120},  // sleep out, 120 ms
    {0x29, (uint8_t[]){0x00}, 1, 20},   // display on, 20 ms
};

/* ------------------------------------------------------------------------ */
/* Shared I2C bus (GT911 touch, ES8311 codec)                               */
/* ------------------------------------------------------------------------ */

static i2c_master_bus_handle_t s_i2c;

static esp_err_t i2c_bus(i2c_master_bus_handle_t *bus)
{
    if (!s_i2c) {
        const i2c_master_bus_config_t cfg = {
            .i2c_port = -1, /* any free port */
            .sda_io_num = BOARD_PIN_I2C_SDA,
            .scl_io_num = BOARD_PIN_I2C_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &s_i2c), TAG, "i2c bus");
    }
    *bus = s_i2c;
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Backlight (LEDC PWM)                                                     */
/* ------------------------------------------------------------------------ */

static int s_bl_level = -1;

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BL_DUTY_BITS,
        .timer_num = BL_LEDC_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc timer");
    const ledc_channel_config_t ch = {
        .gpio_num = BOARD_PIN_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BL_LEDC_CHANNEL,
        .timer_sel = BL_LEDC_TIMER,
        .duty = 0,
    };
    return ledc_channel_config(&ch);
}

void board_set_brightness(int level)
{
    level = level < 0 ? 0 : level > BOARD_BRIGHTNESS_MAX ? BOARD_BRIGHTNESS_MAX : level;
    if (level == s_bl_level) {
        return;
    }
    /* Quadratic: the eye's response is far from linear, so the low levels need small steps */
    const uint32_t max = (1u << BL_DUTY_BITS) - 1, full = BOARD_BRIGHTNESS_MAX * BOARD_BRIGHTNESS_MAX;
    uint32_t duty = level ? (max * level * level + full - 1) / full : 0;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL);
    s_bl_level = level;
}

int board_get_brightness(void)
{
    return s_bl_level < 0 ? 0 : s_bl_level;
}

/* ------------------------------------------------------------------------ */
/* Display + touch                                                          */
/* ------------------------------------------------------------------------ */

/**
 * DMA2D copy into the frame buffer finished. Replaces esp_lvgl_port's callback: with CONFIG_LCD_DSI_ISR_CACHE_SAFE
 * (needed so flash writes don't make the screen flash, see sdkconfig.defaults.esp32p4) the DPI driver only
 * accepts callbacks in IRAM, and lvgl_port's isn't (its registration fails silently, and the screen would freeze).
 * Does what lv_display_flush_ready() does; that function isn't in IRAM either.
 */
static IRAM_ATTR bool flush_done_cb(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata, void *ctx)
{
    ((lv_display_t *)ctx)->flushing = 0;
    return false;
}

static esp_err_t touch_init(lv_display_t *disp)
{
    i2c_master_bus_handle_t bus;
    ESP_RETURN_ON_ERROR(i2c_bus(&bus), TAG, "i2c");
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.scl_speed_hz = 400000;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io), TAG, "touch io");
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_LCD_H_RES,
        .y_max = BOARD_LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
    };
    esp_lcd_touch_handle_t tp = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_gt911(io, &tp_cfg, &tp), TAG, "GT911");
    const lvgl_port_touch_cfg_t touch_cfg = {.disp = disp, .handle = tp};
    ESP_RETURN_ON_FALSE(lvgl_port_add_touch(&touch_cfg), ESP_FAIL, TAG, "lvgl touch");
    return ESP_OK;
}

esp_err_t board_display_init(lv_display_t **disp)
{
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight");

    /* MIPI D-PHY supply */
    esp_ldo_channel_handle_t ldo = NULL;
    const esp_ldo_channel_config_t ldo_cfg = {.chan_id = MIPI_PHY_LDO_CHAN, .voltage_mv = MIPI_PHY_LDO_MV};
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &ldo), TAG, "MIPI LDO");

    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = 2,
        .task_stack = 8 * 1024,
        .task_affinity = 1,
        .task_max_sleep_ms = 50,
        .timer_period_ms = 5,
    };
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "lvgl port");

    esp_lcd_dsi_bus_handle_t dsi = NULL;
    const esp_lcd_dsi_bus_config_t bus_cfg = JD9165_PANEL_BUS_DSI_2CH_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &dsi), TAG, "DSI bus");
    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_dbi_io_config_t dbi_cfg = {.virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8};
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(dsi, &dbi_cfg, &io), TAG, "DBI io");

    const esp_lcd_dpi_panel_config_t dpi_cfg = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 52,
        .virtual_channel = 0,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 1,
        .video_timing = {
            .h_size = BOARD_LCD_H_RES,
            .v_size = BOARD_LCD_V_RES,
            .hsync_back_porch = 136,
            .hsync_pulse_width = 24,
            .hsync_front_porch = 160,
            .vsync_back_porch = 21,
            .vsync_pulse_width = 2,
            .vsync_front_porch = 12,
        },
    };
    const jd9165_vendor_config_t vendor_cfg = {
        .init_cmds = lcd_cmd,
        .init_cmds_size = sizeof(lcd_cmd) / sizeof(lcd_cmd[0]),
        .mipi_config = {.dsi_bus = dsi, .dpi_config = &dpi_cfg},
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BOARD_PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&vendor_cfg,
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_jd9165(io, &panel_cfg, &panel), TAG, "JD9165");
    /* The JD9165 driver returns the DPI panel itself: draw_bitmap copies into the frame buffer with DMA2D */
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_enable_dma2d(panel), TAG, "DMA2D");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "init");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = BOARD_LCD_H_RES * LVGL_BUFFER_LINES,
        .double_buffer = true,
        .hres = BOARD_LCD_H_RES,
        .vres = BOARD_LCD_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_spiram = true,
        },
    };
    const lvgl_port_display_dsi_cfg_t dsi_cfg = {.flags.avoid_tearing = false};
    esp_log_level_set("lcd.dsi", ESP_LOG_NONE); /* lvgl_port's callback registration fails (not in IRAM), see below */
    lv_display_t *d = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    esp_log_level_set("lcd.dsi", ESP_LOG_INFO);
    ESP_RETURN_ON_FALSE(d, ESP_FAIL, TAG, "lvgl add disp");
    const esp_lcd_dpi_panel_event_callbacks_t cbs = {.on_color_trans_done = flush_done_cb};
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_register_event_callbacks(panel, &cbs, d), TAG, "DPI callbacks");
    if (disp) {
        *disp = d;
    }

    esp_err_t err = touch_init(d);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Touch not available: %s", esp_err_to_name(err)); /* the player still works */
    }
    board_set_brightness(BOARD_BRIGHTNESS_MAX);
    ESP_LOGI(TAG, "Display initialized (%dx%d)", BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Ethernet: EMAC (RMII) + IP101GR PHY (generic IEEE 802.3 driver)          */
/* ------------------------------------------------------------------------ */

esp_err_t board_eth_init(esp_eth_handle_t *eth)
{
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_cfg.interface = EMAC_DATA_INTERFACE_RMII;
    emac_cfg.smi_gpio.mdc_num = BOARD_PIN_ETH_MDC;
    emac_cfg.smi_gpio.mdio_num = BOARD_PIN_ETH_MDIO;
    emac_cfg.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN; /* 50 MHz from the PHY side */
    emac_cfg.clock_config.rmii.clock_gpio = BOARD_PIN_ETH_REF_CLK;
    emac_cfg.emac_dataif_gpio.rmii = (eth_mac_rmii_gpio_config_t){
        .tx_en_num = BOARD_PIN_ETH_TX_EN,
        .txd0_num = BOARD_PIN_ETH_TXD0,
        .txd1_num = BOARD_PIN_ETH_TXD1,
        .crs_dv_num = BOARD_PIN_ETH_CRS_DV,
        .rxd0_num = BOARD_PIN_ETH_RXD0,
        .rxd1_num = BOARD_PIN_ETH_RXD1,
    };
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);

    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = ESP_ETH_PHY_ADDR_AUTO;
    phy_cfg.reset_gpio_num = BOARD_PIN_ETH_RST;
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_cfg);

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
    /* PHY ID (registers 2/3: OUI, model, revision): 0243:0c54 = IP101GR on the boards seen so far */
    uint32_t addr = 0, id1 = 0, id2 = 0;
    esp_eth_ioctl(*eth, ETH_CMD_G_PHY_ADDR, &addr);
    esp_eth_ioctl(*eth, ETH_CMD_READ_PHY_REG, &(esp_eth_phy_reg_rw_data_t){.reg_addr = 2, .reg_value_p = &id1});
    esp_eth_ioctl(*eth, ETH_CMD_READ_PHY_REG, &(esp_eth_phy_reg_rw_data_t){.reg_addr = 3, .reg_value_p = &id2});
    ESP_LOGI(TAG, "Ethernet PHY at address %"PRIu32", ID %04"PRIx32":%04"PRIx32, addr, id1, id2);
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Audio: ES8311 (I2S slave) + NS4150 amplifier                             */
/* ------------------------------------------------------------------------ */

esp_err_t board_audio_init(void)
{
#if !CONFIG_JC1060_AUDIO_ES8311
    /* PCM5102 on the header: nothing to set up. Keep the speaker amplifier off (no input, it would only hiss). */
    const gpio_config_t pa = {.mode = GPIO_MODE_OUTPUT, .pin_bit_mask = BIT64(BOARD_PIN_PA_EN)};
    gpio_config(&pa);
    gpio_set_level(BOARD_PIN_PA_EN, 0);
    return ESP_OK;
#endif
    i2c_master_bus_handle_t bus;
    ESP_RETURN_ON_ERROR(i2c_bus(&bus), TAG, "i2c");
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
}
