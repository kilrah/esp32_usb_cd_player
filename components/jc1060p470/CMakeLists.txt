idf_component_register(SRCS "jc1060p470.c"
                       INCLUDE_DIRS "include"
                       REQUIRES lvgl esp_lcd esp_eth
                       PRIV_REQUIRES esp_lvgl_port esp_driver_gpio esp_driver_ledc esp_driver_i2c esp_hw_support
                                     esp_lcd_jd9165 esp_lcd_touch_gt911 esp_codec_dev)
