idf_component_register(
    SRCS
    "lvgl_bsp.cpp"
    REQUIRES
    espressif__esp_lvgl_adapter
    port_bsp
    lvgl__lvgl
    esp_timer
    INCLUDE_DIRS 
    "./")

# esp_lv_adapter headers use partial struct initializers in macros;
# GCC 15 promotes -Wmissing-field-initializers to an error via -Werror.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
