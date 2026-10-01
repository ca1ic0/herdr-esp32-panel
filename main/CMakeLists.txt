file(GLOB FONT_SRCS "fonts/*.c")   # generated CJK subset fonts, if present

idf_component_register(SRCS
                       "app_main.cpp"
                       "ui_panel.c"
                       "panel_store.c"
                       "panel_worker.c"
                       "panel_api_client.c"
                       "wifi_connect.c"
                       "app_config.c"
                       "panel_prefs.c"
                       "panel_audio.cpp"
                       "provisioning.c"
                       ${FONT_SRCS}
                       REQUIRES app_bsp port_bsp pmicpower lvgl__lvgl espressif__esp_lvgl_adapter
                       PRIV_REQUIRES esp_http_client espressif__cjson espressif__esp_codec_dev esp_wifi esp_netif esp_event nvs_flash esp_driver_gpio esp_driver_i2s esp_driver_i2c esp_timer esp_http_server lwip esp_hw_support
                       EMBED_FILES "assets/sounds/done.pcm" "assets/sounds/request.pcm"
                       INCLUDE_DIRS ".")
