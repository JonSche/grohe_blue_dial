#pragma once

#include <cstdint>

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

namespace display {

// Minimal, LVGL-free smoke test for the ported lcd_panel_gc9a01 driver
// (components/display/lcd_panel_gc9a01.c): brings up SPI + the panel via
// the exact same driver and vendor init table as Gc9a01Display, draws
// exactly one solid-red 240x240 frame via esp_lcd_panel_draw_bitmap(), then
// sleeps forever. No esp_lvgl_port, no LVGL task, no UI, no repeated draws.
//
// Exists only to isolate whether the panel stays stable without LVGL ever
// entering the picture -- see docs/ARCHITECTURE.md.
class SingleFrameTest {
 public:
  SingleFrameTest() = default;
  ~SingleFrameTest();

  SingleFrameTest(const SingleFrameTest&) = delete;
  SingleFrameTest& operator=(const SingleFrameTest&) = delete;

  // Brings up SPI/panel, draws exactly one red frame, turns the backlight
  // on, and returns. Never calls draw_bitmap again after this.
  esp_err_t Init();

  // Never returns -- sleeps forever, no further panel activity.
  [[noreturn]] void Run();

 private:
  esp_lcd_panel_io_handle_t panel_io_ = nullptr;
  esp_lcd_panel_handle_t panel_ = nullptr;
  uint16_t* frame_buffer_ = nullptr;
  bool spi_bus_initialized_ = false;
};

}  // namespace display
