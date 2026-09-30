idf_component_register(SRCS "app_main.cpp" "ui_panel.c" "panel_state.c" "wifi_connect.c" "herdr_client.c" "app_config.c" "provisioning.c"
                       REQUIRES app_bsp port_bsp pmicpower lvgl__lvgl espressif__esp_lvgl_adapter
                       PRIV_REQUIRES esp_http_client espressif__cjson esp_wifi esp_netif esp_event nvs_flash esp_driver_gpio esp_timer esp_http_server lwip
                       INCLUDE_DIRS ".")
