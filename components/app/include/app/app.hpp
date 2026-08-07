#pragma once

#include "app/dial_controller.hpp"
#include "display/gc9a01_display.hpp"
#include "encoder/encoder_input.hpp"
#include "grohe_ble/grohe_client.hpp"
#include "time_service/wifi_connection.hpp"
#include "time_service/wifi_credentials.hpp"
#include "ui/ui_manager.hpp"

namespace app {

// Composition root: owns every subsystem and wires them together. This is
// the one place that knows about all of display/, encoder/, ui/,
// grohe_ble/, and time_service/'s Wi-Fi connection, which keeps those
// components decoupled from each other.
//
// App itself contains no interaction rules: it only polls EncoderInput for
// events, hands them to DialController, and re-renders UiManager from the
// resulting DialState. All business logic lives in DialController.
//
// GroheClient is polled the same way: App::Run()'s loop drains its event
// queue every iteration, on this same task -- never from a BLE callback
// (see grohe_ble/ble_manager.hpp). Lifecycle events are still just logged
// (unchanged since M3.1); the appliance's decoded protocol response also
// reaches DialState via DialController::HandleApplianceState() (M7) -- the
// same "App hands GroheClient's output to DialController" pattern the
// encoder loop already uses. As of M8, App is also the one place that
// turns a DialAction (HandleEvent()'s result) into an actual
// GroheClient::RequestDispense()/RequestStop() call, and feeds the
// resulting CommandOutcome back into DialController -- DialController
// itself never touches GroheClient directly, matching how it has never
// touched BleManager directly either.
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

  // The one Wi-Fi connection this firmware ever brings up, used by
  // grohe_client_'s own SntpTimeProvider (a one-shot SNTP time source) --
  // SntpTimeProvider doesn't own Wi-Fi itself, it takes a reference to
  // this instance instead, the same dependency-injection pattern already
  // used one level down (compare wifi_credentials_provider_ ->
  // wifi_connection_ here to how credentials_provider_ -> time_provider_
  // already works inside GroheClient). Declared before grohe_client_ so it
  // can take it by reference in its own constructor (member init order
  // follows declaration order, not the constructor-argument order below).
  // Reference-counted rather than assuming exactly one consumer -- see
  // wifi_connection.hpp's own comment -- even though SntpTimeProvider is
  // currently the only one; a former second consumer (an OTA update
  // engine) was removed, but the design still holds for any future one.
  time_service::LocalWifiCredentialsProvider wifi_credentials_provider_;
  time_service::WifiConnection wifi_connection_{wifi_credentials_provider_};

  grohe_ble::GroheClient grohe_client_{wifi_connection_};
};

}  // namespace app
