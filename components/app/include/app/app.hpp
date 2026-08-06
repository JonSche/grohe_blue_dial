#pragma once

#include "app/dial_controller.hpp"
#include "display/gc9a01_display.hpp"
#include "encoder/encoder_input.hpp"
#include "grohe_ble/grohe_client.hpp"
#include "ota/ota_manager.hpp"
#include "time_service/wifi_connection.hpp"
#include "time_service/wifi_credentials.hpp"
#include "ui/ui_manager.hpp"

namespace app {

// Composition root: owns every subsystem and wires them together. This is
// the one place that knows about all of display/, encoder/, ui/,
// grohe_ble/, ota/, and (M12.5) time_service/'s Wi-Fi connection, which
// keeps those components decoupled from each other -- ota:: in particular
// has no idea any of display/encoder/ui/grohe_ble exist (see
// ota_manager.hpp's own comment); it and grohe_client_'s own
// SntpTimeProvider only share time_service::WifiConnection, nothing else.
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

  // M12.5: the one Wi-Fi connection this firmware ever brings up, shared
  // by grohe_client_'s own SntpTimeProvider (a one-shot SNTP time source)
  // and ota_ (an HTTPS OTA download) -- neither owns Wi-Fi itself
  // anymore; both take a reference to this instance instead, exactly the
  // dependency-injection pattern already used one level down (compare
  // wifi_credentials_provider_ -> wifi_connection_ here to how
  // credentials_provider_ -> time_provider_ already worked inside
  // GroheClient pre-M12.5). Declared before grohe_client_/ota_ so both
  // can take it by reference in their own constructors (member init order
  // follows declaration order, not the constructor-argument order below).
  // See docs/ARCHITECTURE.md#wifi-ownership-m125 for the full design.
  time_service::LocalWifiCredentialsProvider wifi_credentials_provider_;
  time_service::WifiConnection wifi_connection_{wifi_credentials_provider_};

  grohe_ble::GroheClient grohe_client_{wifi_connection_};

  // M12.4: owned here (the composition root), like every other subsystem,
  // but Run() only ever calls Init() on it -- CheckForUpdate()/
  // StartUpdate() have no caller yet in this milestone (no automatic
  // update checks, no background task; see ota_manager.hpp's own class
  // comment). A future Home Assistant integration (M13) or a manual
  // trigger calls them later, through this same instance.
  ota::OtaManager ota_{wifi_connection_};
};

}  // namespace app
