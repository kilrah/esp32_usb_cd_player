idf_component_register(SRCS "cd_player_main.c" "player.c" "audio_out.c" "ui.c" "net.c" "ota.c" "storage.c" "web.c" "meta.c" "discid.c" "captive_dns.c"
                    INCLUDE_DIRS ""
                    EMBED_TXTFILES "web/index.html"
                    PRIV_REQUIRES usb_host_atapi tdisplay_s3 esp_driver_i2s esp_driver_uart esp_driver_gpio esp_driver_pcnt esp_timer nvs_flash
                                  esp_lvgl_port lvgl
                                  esp_wifi esp_netif esp_event esp_http_server app_update esp_app_format esp_partition esp_http_client mbedtls
                    )
