#pragma once

#include "display/gc9a01_display.hpp"
#include "encoder/button.hpp"
#include "encoder/rotary_encoder.hpp"
#include "ui/ui_manager.hpp"

namespace app {

// Composition root: owns every subsystem and wires them together. This is
// the one place that knows about all of display/, encoder/ and ui/, which
// keeps those components decoupled from each other. A future BLE client
// (e.g. GroheBleClient) plugs in here as one more member, without any of the
// existing components needing to change.
class App {
 public:
  App() = default;

  // Brings up all subsystems and runs the application loop. Never returns.
  [[noreturn]] void Run();

 private:
  void PollInputs();

  display::Gc9a01Display display_;
  ui::UiManager ui_;
  encoder::RotaryEncoder encoder_;
  encoder::Button button_;

  int32_t last_encoder_position_ = 0;
  bool last_button_pressed_ = false;
};

}  // namespace app
