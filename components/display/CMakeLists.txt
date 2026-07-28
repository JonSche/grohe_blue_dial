idf_component_register(
    SRCS "gc9a01_display.cpp" "lcd_panel_gc9a01.c" "single_frame_test.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board gc9a01_vendor esp_lcd_gc9a01 esp_lvgl_port lvgl driver esp_lcd esp_timer freertos log
)
