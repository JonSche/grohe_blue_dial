#include "display/single_frame_test.hpp"
#include "esp_err.h"

// TEMPORARY diagnostic entry point: isolates whether the ported
// lcd_panel_gc9a01 driver stays stable with a single draw_bitmap() call and
// no LVGL task ever running (no esp_lvgl_port, no UI). See
// docs/ARCHITECTURE.md. app::App (the real LVGL-based entry point) is
// untouched under components/app and this should be reverted back to it
// once this experiment is done.
extern "C" void app_main() {
  static display::SingleFrameTest test;
  ESP_ERROR_CHECK(test.Init());
  test.Run();
}
