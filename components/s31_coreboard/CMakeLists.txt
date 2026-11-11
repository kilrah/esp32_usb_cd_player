idf_component_register(SRCS "s31_coreboard.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_eth
                       PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_codec_dev)
