#pragma once

#include "dial_state/dial_state.hpp"
#include "lvgl.h"

namespace ui {

// Builds and owns the LVGL screen tree for the Grohe Dial's main screen: a
// circular progress ring showing the pour amount, the amount itself, the
// selected water type, and a static hint. Every widget is created once in
// Init(); Render() only ever updates their content/value from the given
// DialState -- it never contains business logic, and DialState (not any
// widget) is the single source of truth.
//
// Callers must hold the LVGL lock (display::Gc9a01Display::Lock()) for the
// duration of Init() and Render(), since LVGL itself isn't thread-safe.
class UiManager {
 public:
  UiManager() = default;

  // Builds the dial screen on the given LVGL display. Widgets start empty;
  // call Render() immediately after to populate them from the real state.
  void Init(lv_display_t* display);

  // Updates every widget to reflect state. Safe to call repeatedly; this is
  // the only way the screen's content ever changes.
  void Render(const dial_state::DialState& state);

 private:
  lv_obj_t* arc_ = nullptr;
  lv_obj_t* amount_label_ = nullptr;
  lv_obj_t* water_type_label_ = nullptr;
  // M2's static hint, now updated by Render() (M8) to reflect
  // dispense_status: "PRESS TO POUR" while idle, "PRESS TO STOP" while
  // dispensing -- reusing the existing label rather than adding a new one,
  // per "no UI redesign".
  lv_obj_t* hint_label_ = nullptr;
  // M7: the one UI addition for the appliance's decoded protocol state --
  // see Render()'s own implementation for exactly what it shows.
  lv_obj_t* appliance_status_label_ = nullptr;
};

}  // namespace ui
