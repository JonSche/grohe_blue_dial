#pragma once

#include "app/dial_controller.hpp"
#include "display/gc9a01_display.hpp"
#include "encoder/encoder_input.hpp"
#include "ui/ui_manager.hpp"

namespace app {

// Composition root: owns every subsystem and wires them together. This is
// the one place that knows about all of display/, encoder/ and ui/, which
// keeps those components decoupled from each other. A future BLE client
// (e.g. GroheBleClient) plugs in here as one more member, without any of the
// existing components needing to change.
//
// App itself contains no interaction rules: it only polls EncoderInput for
// events, hands them to DialController, and re-renders UiManager from the
// resulting DialState. All business logic lives in DialController.
class App {
 public:
  App() = default;

  // Brings up all subsystems and runs the application loop. Never returns.
  [[noreturn]] void Run();

 private:
  display::Gc9a01Display display_;
  ui::UiManager ui_;
  encoder::EncoderInput encoder_input_;
  DialController dial_controller_;
};

}  // namespace app
