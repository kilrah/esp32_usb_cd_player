idf_component_register(SRCS "tdisplay_s3.c"
                       INCLUDE_DIRS "include"
                       REQUIRES lvgl esp_lcd
                       PRIV_REQUIRES esp_lvgl_port esp_driver_gpio)
