#pragma once

#include <string_view>

#include "lvgl.h"

namespace ui {

// Builds and owns the LVGL screen tree. All widgets that later milestones add
// (dial gauge, BLE connection state, ...) get created through this class so
// app::App never touches LVGL objects directly.
//
// Callers must hold the LVGL port lock (display::Gc9a01Display::Lock()) for
// the duration of Init() and any Set*() call, since LVGL itself isn't
// thread-safe.
class UiManager {
 public:
  UiManager() = default;

  // Builds the boot screen ("Grohe Dial" centered on the active display) on
  // the given LVGL display.
  void Init(lv_display_t* display);

  // Updates the status text shown below the title. Safe to call repeatedly
  // once Init() has run; reserved for future milestones (e.g. BLE state).
  void SetStatusText(std::string_view text);

 private:
  lv_obj_t* title_label_ = nullptr;
  lv_obj_t* status_label_ = nullptr;
};

}  // namespace ui
