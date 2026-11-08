idf_component_register(SRCS "cd_player_main.c" "player.c" "audio_out.c" "ui.c"
                    INCLUDE_DIRS ""
                    PRIV_REQUIRES usb_host_atapi tdisplay_s3 esp_driver_i2s esp_driver_uart esp_driver_gpio esp_driver_pcnt esp_timer
                                  esp_lvgl_port lvgl
                    )
