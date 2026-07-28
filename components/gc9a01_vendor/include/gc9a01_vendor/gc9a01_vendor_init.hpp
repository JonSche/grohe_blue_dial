#pragma once

#include "esp_lcd_gc9a01.h"

namespace gc9a01_vendor {

// Manufacturer-verified GC9A01 vendor init sequence for the VIEWE
// UEDX24240013-MD50E-B panel, copied byte-for-byte from the manufacturer's
// working ESP-IDF reference (components/bsp/lcd_panel_gc9a01.c in the VIEWE
// SDK). The esp_lcd_gc9a01 component's built-in vendor_specific_init_default
// is a generic sequence that omits this panel's power-control registers
// (0xb5/0xb6/0xbd/0xba/0xbc) and Display Inversion On (0x21), and uses
// different VGH/VGL bias bytes in 0x62/0x63 -- without it the panel accepts
// every SPI command without error but never shows contrast (see
// docs/display_diff.md). Pass this via
// esp_lcd_panel_dev_config_t::vendor_config before calling
// esp_lcd_new_panel_gc9a01() so every caller (bring-up and the LVGL display
// path alike) programs the panel identically.
extern const gc9a01_vendor_config_t kGc9a01VendorConfig;

}  // namespace gc9a01_vendor
