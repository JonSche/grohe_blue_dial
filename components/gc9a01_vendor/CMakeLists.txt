# Manufacturer-verified GC9A01 vendor init sequence, shared by the raw
# bring-up test and the LVGL display path. Only depends on esp_lcd_gc9a01
# -- no LVGL, no other local component.
idf_component_register(
    SRCS "gc9a01_vendor_init.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd_gc9a01
)
