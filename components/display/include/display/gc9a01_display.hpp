#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
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
  // registers a self-owned LVGL v9 display (frame buffers, flush callback,
  // tick timer, dedicated task). Safe to call exactly once.
  esp_err_t Init();

  // The LVGL display object created by Init(), for handing to ui::UiManager.
  // Valid only after a successful Init().
  [[nodiscard]] lv_display_t* LvDisplay() const { return lv_display_; }

  void SetBacklight(bool on) const;

  // Acquire/release the recursive mutex guarding LVGL access from outside
  // the dedicated LVGL task. Every LVGL call made from app/ui code that
  // isn't already inside an LVGL callback must be wrapped in Lock()/Unlock().
  [[nodiscard]] static bool Lock(uint32_t timeout_ms = 0);
  static void Unlock();

 private:
  // Runs on the dedicated LVGL task; calls lv_timer_handler() under
  // Lock()/Unlock() on a fixed period.
  static void TaskLoop(void* arg);

  esp_lcd_panel_io_handle_t panel_io_ = nullptr;
  esp_lcd_panel_handle_t panel_ = nullptr;
  lv_display_t* lv_display_ = nullptr;
  bool spi_bus_initialized_ = false;

  // Owned by the LVGL glue set up in Init(); torn down in ~Gc9a01Display().
  // Small row-based partial draw buffer (LV_DISPLAY_RENDER_MODE_PARTIAL),
  // not a full-screen framebuffer -- see the M3.2 architecture review and
  // docs/ARCHITECTURE.md. LVGL runs single-buffered here too
  // (lv_display_set_buffers()'s second buffer argument is optional),
  // waiting for this buffer's flush to complete before rendering the next
  // chunk.
  void* lv_buf_ = nullptr;
  SemaphoreHandle_t lock_mutex_ = nullptr;
  TaskHandle_t lvgl_task_ = nullptr;
  esp_timer_handle_t lv_tick_timer_ = nullptr;

  // Lock()/Unlock() are static (existing public API), but the mutex they
  // guard is per-instance state, matching how panel_/panel_io_ are already
  // modeled. Gc9a01Display is only ever used as a single, process-lifetime
  // instance (see class comment above), so a static back-pointer set once
  // in Init() is enough to let the static methods reach that instance
  // state.
  static Gc9a01Display* instance_;
};

}  // namespace display
