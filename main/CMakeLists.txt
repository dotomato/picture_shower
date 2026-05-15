idf_component_register(
    SRCS main.c axp2101.cpp status_bar.c wifi_manager.c network.c ui_screens.c gravity_ball.c touch_input.c audio.c simhei_tomato.c sthuop_tomato.c
    INCLUDE_DIRS .
    REQUIRES esp_http_client nvs_flash esp_wifi esp_netif esp_event spiffs espressif__esp_new_jpeg esp_timer XPowersLib)

# Pack assert/ folder into the SPIFFS 'storage' partition
# spiffs_create_partition_image(storage ../assert FLASH_IN_PROJECT)
