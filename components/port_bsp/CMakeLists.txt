idf_component_register(
    SRCS 
    "i2c_bsp.cpp" 
    "display_bsp.cpp" 
    REQUIRES 
    esp_driver_gpio
    esp_driver_spi
    esp_driver_i2c
    esp_lcd
    waveshare__esp_lcd_touch_cst9217
    PRIV_REQUIRES 
    pmicpower
    espressif__esp_lcd_sh8601
    esp_timer
    INCLUDE_DIRS 
    "./")

# Vendor board code + touch driver macro use partial struct initializers;
# GCC 15 promotes -Wmissing-field-initializers to an error via -Werror.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
