#pragma once

#include "app/dial_controller.hpp"
#include "display/gc9a01_display.hpp"
#include "encoder/encoder_input.hpp"
#include "grohe_ble/grohe_client.hpp"
#include "grohe_ble/grohe_credentials.hpp"
#include "ota/ota_secret.hpp"
#include "ota/ota_server.hpp"
#include "provisioning/provisioning_secret.hpp"
#include "provisioning/provisioning_server.hpp"
#include "settings/dial_settings.hpp"
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

  // M13.1: persisted (NVS-backed) default amount/encoder step/default
  // water type -- see components/settings/. Independent of Wi-Fi/BLE;
  // Init() runs early in App::Run(), and its Values() are fed into
  // dial_controller_ via ApplySettings() once, before the first render,
  // so the very first frame already reflects any stored settings rather
  // than dial_state.hpp's compile-time ones getting shown and then
  // silently replaced.
  settings::DialSettingsStore dial_settings_;

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
  // wifi_connection.hpp's own comment. Two consumers today:
  // SntpTimeProvider's one-shot boot-time burst (via grohe_client_ below)
  // and ota_server_'s permanent, never-released acquisition (M12) -- the
  // design already anticipated exactly this second case.
  time_service::LocalWifiCredentialsProvider wifi_credentials_provider_;
  time_service::WifiConnection wifi_connection_{wifi_credentials_provider_};

  // M12: local-network Wi-Fi OTA endpoint -- see components/ota/ and
  // docs/ARCHITECTURE.md's "OTA" section. Independent of BLE/dispensing;
  // Init() is non-blocking and never gates the rest of startup.
  ota::LocalOtaSecretProvider ota_secret_provider_;
  ota::OtaServer ota_server_{wifi_connection_, ota_secret_provider_};

  // M13.1: NVS-backed BLE credentials, falling back to the existing
  // gitignored-local-header developer credentials whenever nothing has
  // been provisioned -- see grohe_credentials.hpp's own comment.
  // Declared before grohe_client_/provisioning_server_ so it can be
  // passed by reference to both of their constructors (member init order
  // follows declaration order, not the constructor-argument order
  // below).
  grohe_ble::NvsCredentialsProvider grohe_credentials_provider_;

  // M13.2: local-network HTTP provisioning endpoint -- see
  // components/provisioning/ and docs/ARCHITECTURE.md's "Provisioning
  // (M13.2)" section. Independent of BLE/dispensing; Init() is
  // non-blocking and never gates the rest of startup, same shape as
  // ota_server_ above -- a deliberately *separate* secret
  // (provisioning_secret_provider_, never ota_secret_provider_) and a
  // deliberately separate esp_http_server instance/port from it too
  // (components/ota/ itself is untouched by this milestone).
  provisioning::LocalProvisioningSecretProvider provisioning_secret_provider_;
  provisioning::ProvisioningServer provisioning_server_{
      wifi_connection_, provisioning_secret_provider_,
      grohe_credentials_provider_};

  grohe_ble::GroheClient grohe_client_{wifi_connection_, grohe_credentials_provider_};
};

}  // namespace app
