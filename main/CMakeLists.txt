# Board support component (provides board.h), one per target; see main/idf_component.yml and CMakePresets.json.
# Kconfig values aren't available yet when requirements are resolved, so the target decides.
# A board without a display (BOARD_HAS_DISPLAY 0 in its board.h) builds without the UI and LVGL.
set(console_req "")
set(has_display ON)
if(IDF_TARGET STREQUAL "esp32p4")
    set(board jc1060p470)
    set(console_req esp_driver_usb_serial_jtag)
elseif(IDF_TARGET STREQUAL "esp32s31")
    set(board s31_coreboard)
    set(has_display OFF)
else()
    set(board tdisplay_s3)
endif()

set(ui_srcs "")
set(ui_reqs "")
set(fonts "")
if(has_display)
    set(fonts "fonts/font_text_14.c" "fonts/font_text_20.c" "fonts/font_text_28.c" "fonts/font_text_40.c")
    set(ui_srcs "ui.c" "ui_layout_small.c" "ui_layout_large.c" ${fonts})
    set(ui_reqs esp_lvgl_port lvgl)
endif()

idf_component_register(SRCS "cd_player_main.c" "player.c" "audio_out.c" "net.c" "ota.c" "storage.c" "web.c" "meta.c" "meta_io.c" "discid.c" "captive_dns.c" "auth.c"
                            ${ui_srcs}
                    INCLUDE_DIRS ""
                    EMBED_TXTFILES "web/index.html"
                    PRIV_REQUIRES esp_usb_host_atapi ${board} esp_driver_i2s esp_driver_uart esp_driver_gpio esp_driver_pcnt esp_timer nvs_flash
                                  ${console_req} ${ui_reqs}
                                  esp_wifi esp_netif esp_event esp_http_server app_update esp_app_format esp_partition esp_http_client mbedtls
                    )

# Generated fonts (font_conv.ps1) include "lvgl.h" only with this defined. Fonts a layout doesn't use are
# dropped by the linker.
if(fonts)
    set_source_files_properties(${fonts} PROPERTIES COMPILE_DEFINITIONS LV_LVGL_H_INCLUDE_SIMPLE)
endif()
