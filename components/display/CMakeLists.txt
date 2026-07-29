idf_component_register(
    SRCS "gc9a01_display.cpp" "lcd_panel_gc9a01.c"
    INCLUDE_DIRS "include"
    REQUIRES board gc9a01_vendor esp_lcd_gc9a01 lvgl driver esp_lcd esp_timer freertos log
)
