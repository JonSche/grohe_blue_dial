#pragma once

#include <cstdint>

#include "app/dispense_session.hpp"
#include "dial_state/dial_state.hpp"
#include "encoder/encoder_input.hpp"

// Forward-declared rather than #include "grohe_ble/grohe_protocol.hpp" /
// "grohe_ble/grohe_client.hpp": those headers transitively pull in
// ble_manager.hpp -> nimble/ble.h -> os/os.h, which #defines min/max as
// macros -- fine for grohe_ble's own files, but this header is included by
// dial_controller.cpp, which calls std::min/std::max. Every method below
// only needs these types by const reference or by value (WaterType, a
// plain enum), so forward declarations are enough here; the .cpp includes
// the real headers where it accesses fields/enumerators.
namespace grohe_ble {
struct ApplianceState;
struct CommandOutcome;
enum class WaterType;
}  // namespace grohe_ble

namespace app {

// What HandleEvent() decided should happen, for App::Run() to act on --
// DialController itself never touches GroheClient (see grohe_client.hpp:
// App is the composition root that bridges DialController and GroheClient,
// the same role it already plays for HandleApplianceState()/
// HandleCommandOutcome() below).
enum class DialAction {
  kNone,
  kRequestDispense,
  kRequestStop,
};

// The application's interaction rules: owns the single DialState instance
// and is the only thing allowed to mutate it. Translates encoder events into
// state changes (amount stepping/clamping, water-type toggling) and, as of
// M8, the actual dispense/stop state machine -- see HandleEvent()'s and
// HandleCommandOutcome()'s own comments. Contains no LVGL or UI code --
// ui::UiManager only ever reads the resulting State() and renders it.
class DialController {
 public:
  // Idle: short press -> kRequestDispense (unless a command is already in
  // flight -- see command_pending_'s own comment). Dispensing: short press
  // -> kRequestStop. Long press toggles water type, but only while idle
  // (meaningless mid-dispense). Rotation always adjusts amount_ml
  // regardless of status -- harmless, only affects the *next* dispense.
  [[nodiscard]] DialAction HandleEvent(encoder::EncoderEvent event);

  // Called by App right after it attempts to actually send the command
  // HandleEvent() requested; `accepted` is whatever
  // GroheClient::RequestDispense()/RequestStop() returned. Marks a command
  // as in flight only if it was actually accepted, so a rejected request
  // doesn't block the very next press.
  void HandleCommandSent(bool accepted);

  // The only place a completed command is interpreted: a successful
  // dispense acknowledgement enters Dispensing and starts dispense_session_
  // (using the amount as it was when the command was sent); a successful
  // stop acknowledgement returns to Idle immediately. A non-success outcome
  // (INVALID_HMAC, etc.) leaves dispense_status as it was -- graceful, not
  // invented. Always clears the in-flight flag. Returns whether anything
  // changed, so App::Run() only re-renders when it needs to.
  [[nodiscard]] bool HandleCommandOutcome(
      const grohe_ble::CommandOutcome& outcome);

  // Called every App::Run() iteration: returns to Idle once
  // dispense_session_ reports the predicted duration has elapsed. Returns
  // whether anything changed.
  [[nodiscard]] bool Tick();

  // Called on BleEventType::kConnectionFailed: forces a return to Idle and
  // clears in-flight state, satisfying "disconnect during dispense" from
  // this milestone's error-handling requirements. Returns whether anything
  // changed.
  [[nodiscard]] bool HandleConnectionLost();

  // Translates the appliance's decoded protocol response into DialState's
  // own plain fields -- dial_state has no dependency on grohe_ble (see
  // dial_state.hpp), so this is the one place that bridges the two. Purely
  // diagnostic display (the "APPL ..." status label) -- independent of the
  // command-outcome path above, which is what actually drives the
  // Idle/Dispensing state machine. Returns true if anything actually
  // changed.
  [[nodiscard]] bool HandleApplianceState(
      const grohe_ble::ApplianceState& appliance_state);

  // Translates GroheClient::HasValidTime() into DialState's own plain
  // field (M9) -- same "dial_state has no dependency on grohe_ble/
  // time_service" bridging role as HandleApplianceState(). Returns true if
  // anything actually changed.
  [[nodiscard]] bool HandleTimeStatus(bool available);

  [[nodiscard]] const dial_state::DialState& State() const { return state_; }

 private:
  dial_state::DialState state_{};
  DispenseSession dispense_session_;

  // True from the moment HandleCommandSent(true) runs until
  // HandleCommandOutcome() resolves it -- debounces a rapid second press
  // before the first command's acknowledgement arrives. Distinct from
  // dial_state::DispenseStatus: a dispense *request* is in flight before
  // the appliance has even acknowledged it, let alone started dispensing.
  bool command_pending_ = false;

  // Snapshot of state_.amount_ml at the moment HandleEvent() returned
  // kRequestDispense -- the amount actually sent, not whatever the dial
  // may have since rotated to while the request was in flight.
  // HandleCommandOutcome() starts dispense_session_ from this, not from
  // state_.amount_ml directly.
  int pending_dispense_amount_ml_ = 0;

  // Last ApplianceState::sequence seen by HandleApplianceState(), so it can
  // detect "is this actually new" with a single comparison instead of
  // diffing every field.
  uint32_t last_seen_sequence_ = 0;
};

// Maps the UI-facing water type to the protocol's numeric taste value --
// the one place dial_state::WaterType and grohe_ble::WaterType meet,
// mirroring how HandleApplianceState() is the one place DialState and
// ApplianceState meet.
[[nodiscard]] grohe_ble::WaterType ToGroheWaterType(dial_state::WaterType type);

}  // namespace app
