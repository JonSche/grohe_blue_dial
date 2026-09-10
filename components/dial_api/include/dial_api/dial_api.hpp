#pragma once

#include "dial_state/dial_state.hpp"
#include "settings/dial_settings.hpp"

// M15: the abstract seam between the dial's local HTTP API
// (components/provisioning/, which owns the httpd instance this API
// lives on) and app::App (the only thing allowed to touch
// app::DialController/grohe_ble::GroheClient -- see dial_api_handler.hpp
// -- err, see DialApiHandler's own comment below). Exists as its own
// tiny component, not folded into either side, purely to avoid a
// circular CMake dependency: components/app/ already REQUIRES
// components/provisioning/ (for ProvisioningServer itself), so
// components/provisioning/ cannot REQUIRES components/app/ back.
// components/dial_api/ requires neither -- only dial_state/ and
// settings/, both already dependency-free leaves in this project's own
// graph (see their own CMakeLists.txt comments) -- so both app and
// provisioning can depend on it without a cycle.
namespace dial_api {

// What a command request actually resolved to -- mirrors
// grohe_ble::GroheClient::RequestDispense()/RequestStop()'s own
// "accepted/not accepted" contract (queued for the BLE write, not yet
// acknowledged by the appliance -- that arrives asynchronously, visible
// via Status().dispense_status, exactly like the physical dial's own
// UI) plus the specific reasons app::DialController::HandleEvent()
// already distinguishes for its own encoder-driven callers, now surfaced
// to an HTTP caller instead of silently folded into one boolean.
enum class RequestResult {
  kAccepted,
  // Either a previous request hasn't resolved yet (mirrors
  // DialController's own command_pending_ debounce), or the dial isn't
  // in a state this command makes sense in right now (e.g. stop while
  // idle, dispense while already dispensing) -- deliberately not split
  // into two reasons: DialController's own HandleEvent() doesn't
  // distinguish them either at its call sites (both are just "break" --
  // see dial_controller.cpp), and neither would a caller's own retry
  // logic. Call Status() for the current dispense_status if the specific
  // reason matters.
  kRejectedNotAvailable,
  // BLE not ready for protocol writes yet (mirrors
  // GroheClient::RequestDispense()/RequestStop()'s own false-return for
  // this case).
  kRejectedNotReady,
  // The app task did not respond within the bounded wait -- should not
  // happen at App::Run()'s 20 ms loop cadence; treated as a hard error
  // if it ever does, not silently retried.
  kTimeout,
};

// Implemented by app::App (the composition root that already owns
// DialController/GroheClient/DialSettingsStore), injected by reference
// into provisioning::ProvisioningServer's constructor -- the same
// dependency-injection shape as every other cross-component interface
// already in this codebase (grohe_ble::CredentialsProvider,
// provisioning::ProvisioningSecretProvider, ...). Every method here must
// be safe to call from any task, including the httpd task the new
// /api/* handlers run on -- see the concrete implementation's own
// comment for the cross-task hand-off this requires for the two Request*
// methods, mirroring grohe_ble::BleManager::WriteCharacteristic()'s own
// command_queue_ pattern exactly.
class DialApiHandler {
 public:
  virtual ~DialApiHandler() = default;

  // A snapshot copy of the dial's current state -- dial_state::DialState
  // remains the single source of truth (ui::UiManager::Render() reads
  // the exact same struct on the app task); this is a plain read, not a
  // second state store. Returned by value, not by reference: a
  // reference into state the app task can concurrently mutate would be
  // unsafe for a caller on a different task to read from.
  [[nodiscard]] virtual dial_state::DialState Status() const = 0;

  [[nodiscard]] virtual settings::DialSettings Config() const = 0;

  // Persists new_values through the existing settings::DialSettingsStore
  // exactly as-is (same validation, same NVS blob write) -- no new
  // parallel configuration logic. Deliberately does *not* also force the
  // dial's live, currently-displayed amount_ml/water_type to the new
  // defaults (DialController::ApplySettings() -- see its own comment --
  // is a one-time boot-time initializer, not an idempotent live-update
  // call; forcibly overwriting whatever the user may have already
  // rotated the encoder to, mid-interaction, purely because a *default*
  // changed, would be surprising, not helpful). The new defaults take
  // effect on the next boot, exactly like every other DialSettingsStore
  // consumer today.
  [[nodiscard]] virtual bool SetConfig(
      const settings::DialSettings& new_values) = 0;

  // amount_ml/water_type are validated by the HTTP layer *before* this
  // is called (see provisioning_server.cpp) -- out-of-range/unknown
  // values are rejected there with a precise 422, never silently
  // clamped or defaulted here.
  [[nodiscard]] virtual RequestResult RequestDispense(
      int amount_ml, dial_state::WaterType water_type) = 0;
  [[nodiscard]] virtual RequestResult RequestStop() = 0;
};

}  // namespace dial_api
