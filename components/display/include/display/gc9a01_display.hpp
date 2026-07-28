#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

namespace display {

// Owns the SPI bus, the GC9A01 panel, and its LVGL display driver.
//
// Construction does no I/O; call Init() once during startup. The instance
// must outlive the LVGL display it registers, so keep it alive for the
// lifetime of the application (see app::App).
class Gc9a01Display {
 public:
  Gc9a01Display() = default;
  ~Gc9a01Display();

  Gc9a01Display(const Gc9a01Display&) = delete;
  Gc9a01Display& operator=(const Gc9a01Display&) = delete;

  // Brings up the SPI bus, the panel driver, the backlight GPIO, and
  // registers the panel with esp_lvgl_port. Safe to call exactly once.
  esp_err_t Init();

  // The LVGL display object created by Init(), for handing to ui::UiManager.
  // Valid only after a successful Init().
  [[nodiscard]] lv_display_t* LvDisplay() const { return lv_display_; }

  void SetBacklight(bool on) const;

  // Acquire/release the LVGL mutex that esp_lvgl_port uses to guard access
  // from outside the LVGL task. Every LVGL call made from app/ui code that
  // isn't already inside an LVGL callback must be wrapped in Lock()/Unlock().
  [[nodiscard]] static bool Lock(uint32_t timeout_ms = 0);
  static void Unlock();

 private:
  esp_lcd_panel_io_handle_t panel_io_ = nullptr;
  esp_lcd_panel_handle_t panel_ = nullptr;
  lv_display_t* lv_display_ = nullptr;
  bool spi_bus_initialized_ = false;
};

}  // namespace display
