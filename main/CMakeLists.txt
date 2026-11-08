idf_component_register(SRCS "cd_player_main.c" "status_led.c"
                    INCLUDE_DIRS ""
                    PRIV_REQUIRES usb_host_atapi esp_driver_gpio esp_timer led_strip
                    )
