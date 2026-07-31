#pragma once

#include "app/dial_controller.hpp"
#include "display/gc9a01_display.hpp"
#include "encoder/encoder_input.hpp"
#include "grohe_ble/grohe_client.hpp"
#include "ui/ui_manager.hpp"

namespace app {

// Composition root: owns every subsystem and wires them together. This is
// the one place that knows about all of display/, encoder/, ui/ and
// grohe_ble/, which keeps those components decoupled from each other.
//
// App itself contains no interaction rules: it only polls EncoderInput for
// events, hands them to DialController, and re-renders UiManager from the
// resulting DialState. All business logic lives in DialController.
//
// GroheClient is polled the same way: App::Run()'s loop drains its event
// queue every iteration, on this same task -- never from a BLE callback
// (see grohe_ble/ble_manager.hpp). Lifecycle events are still just logged
// (unchanged since M3.1); as of M7, the appliance's decoded protocol
// response also reaches DialState, via DialController::HandleApplianceState()
// -- the same "App hands GroheClient's output to DialController" pattern
// the encoder loop already uses.
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
  grohe_ble::GroheClient grohe_client_;
};

}  // namespace app
