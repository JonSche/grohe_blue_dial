# Deliberately no lvgl / esp_lvgl_port here: this is the raw esp_lcd
# hardware bring-up test, kept dependency-free of the LVGL UI stack.
# gc9a01_vendor provides the panel init table shared with
# gc9a01_display.cpp and itself depends on nothing but esp_lcd_gc9a01.
idf_component_register(
    SRCS "color_cycle_test.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board gc9a01_vendor esp_lcd_gc9a01 esp_lcd driver freertos log
)
