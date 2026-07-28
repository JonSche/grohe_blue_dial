#pragma once

#include <cstdint>

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

namespace bringup {

// Raw esp_lcd + esp_lcd_gc9a01 hardware bring-up smoke test for the
// UEDX24240013-MD50E-B. Brings up the SPI bus, the GC9A01 panel, and the
// backlight, then fills the whole screen with solid colors in a loop.
//
// Deliberately has no LVGL dependency (no `ui`, no `esp_lvgl_port`): the
// point of this milestone is to validate the panel/SPI/backlight wiring in
// isolation before any UI framework is layered on top. See
// docs/ROADMAP.md.
class ColorCycleTest {
 public:
  ColorCycleTest() = default;
  ~ColorCycleTest();

  ColorCycleTest(const ColorCycleTest&) = delete;
  ColorCycleTest& operator=(const ColorCycleTest&) = delete;

  esp_err_t Init();

  // Cycles red -> green -> blue -> white -> black, one second each. Never
  // returns.
  [[noreturn]] void Run();

 private:
  esp_err_t FillColor(uint16_t rgb565_color) const;

  esp_lcd_panel_io_handle_t panel_io_ = nullptr;
  esp_lcd_panel_handle_t panel_ = nullptr;
  uint16_t* frame_buffer_ = nullptr;
  bool spi_bus_initialized_ = false;
};

}  // namespace bringup
