#pragma once

#include <cstdint>

#include "app/dispense_session.hpp"
#include "dial_state/dial_state.hpp"
#include "encoder/encoder_input.hpp"
#include "settings/dial_settings.hpp"

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
  // M13.1: applies persisted/default settings (default amount, encoder
  // step size, default water type) -- amount_step_ml_ below starts at
  // dial_state.hpp's own compile-time kAmountStepMl, and state_'s
  // amount_ml/water_type start at DialState's own compile-time defaults,
  // so skipping this call entirely (or calling it with
  // settings::DialSettings{}'s own defaults) leaves behavior identical to
  // before this milestone. Intended to be called once, by app::App, right
  // after constructing settings::DialSettingsStore and before the first
  // UiManager::Render() -- see app.cpp. Clamps defensively to
  // dial_state::kMinAmountMl/kMaxAmountMl and a positive step even though
  // settings::DialSettingsStore itself already validates before persisting,
  // since this method doesn't know its argument necessarily came from
  // there.
  void ApplySettings(const settings::DialSettings& settings);

  // Idle: short press -> kRequestDispense (unless a command is already in
  // flight -- see command_pending_'s own comment). Dispensing: short press
  // -> kRequestStop. Long press cycles water type (M10: Still -> Medium ->
  // Sparkling -> Still), but only while idle (meaningless mid-dispense).
  // Rotation always adjusts amount_ml regardless of status -- harmless,
  // only affects the *next* dispense (by amount_step_ml_, see
  // ApplySettings() above).
  [[nodiscard]] DialAction HandleEvent(encoder::EncoderEvent event);

  // M15: additive entry points for a non-encoder trigger (the local
  // HTTP API, components/provisioning/'s new /api/dispense and
  // /api/stop) -- exactly the same debounce/state-machine rules as
  // HandleEvent()'s kShortPress cases (command_pending_ check,
  // dispense_status gate, the same pending_dispense_amount_ml_
  // snapshot/kStopping transition), just parameterized by an explicit
  // amount/water_type instead of reading live encoder-driven state_.
  // amount_ml/water_type are set into state_ first (so the physical
  // dial's own screen reflects an HTTP-triggered dispense exactly as if
  // the encoder had been turned to it -- one single source of truth, no
  // shadow state) -- see dial_controller.cpp for the exact mirroring.
  // Returns kNone for exactly the same reasons HandleEvent() would have
  // returned kNone in the equivalent encoder scenario -- the caller
  // (App, see dial_api::DialApiHandler) maps that to the specific
  // dial_api::RequestResult reason.
  [[nodiscard]] DialAction RequestDispenseAction(int amount_ml,
                                                 dial_state::WaterType water_type);
  [[nodiscard]] DialAction RequestStopAction();

  // Called by App right after it attempts to actually send the command
  // HandleEvent() requested; `accepted` is whatever
  // GroheClient::RequestDispense()/RequestStop() returned. Marks a command
  // as in flight only if it was actually accepted, so a rejected request
  // doesn't block the very next press. If a stop request failed to send
  // (accepted == false), also reverts HandleEvent()'s optimistic kStopping
  // back to kDispensing -- otherwise nothing would ever revert it, since no
  // outcome is coming for a command that was never sent.
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
  // dispense_session_ reports the predicted duration has elapsed. Also the
  // forward-progress fallback for kStopping -- if dispense_session_
  // finishes naturally before a pending stop's acknowledgement ever
  // arrives, this treats it the same as a normal completion rather than
  // leaving the dial stuck on "STOPPING...". (M11.1) Also flips
  // connection_status from kConnectionLost to kConnecting once
  // connection_lost_until_us_ elapses -- BleManager keeps retrying in the
  // background the whole time (see its own backoff schedule); this is
  // purely how long the dial keeps showing "Connection lost" before
  // switching to "Connecting..." for the remainder of that retry.
  // Returns whether anything changed.
  [[nodiscard]] bool Tick();

  // Called on BleEventType::kConnectionFailed: forces a return to Idle and
  // clears in-flight state, satisfying "disconnect during dispense" from
  // M8's error-handling requirements, and (M11) sets connection_status to
  // kConnectionLost. Also clears the ble_ready_for_protocol_/ble_subscribed_
  // flags below, so a subsequent reconnect re-earns kReady from scratch
  // rather than starting from stale flags. (M11.1) Also (re)starts the
  // brief connection_lost_until_us_ hold -- see Tick()'s own comment for
  // why this is what actually produces "Connection lost, then
  // Connecting..." rather than "Connection lost" for the whole retry.
  // Returns whether anything changed.
  [[nodiscard]] bool HandleConnectionLost();

  // Called on BleEventType::kReadyForProtocol / kSubscribed respectively
  // (M11) -- see connection_status's own comment on dial_state.hpp for why
  // DialController tracks these two flags itself instead of GroheClient
  // exposing them. Returns whether anything changed.
  [[nodiscard]] bool HandleReadyForProtocol();
  [[nodiscard]] bool HandleSubscribed();

  // Translates the appliance's decoded protocol response into DialState's
  // own plain fields -- dial_state has no dependency on grohe_ble (see
  // dial_state.hpp), so this is the one place that bridges the two. As of
  // M11.1 this data is log-only (ESP_LOGx, see the .cpp): the UI no longer
  // has a status label for raw response codes -- independent of the
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

  // App-task-only: every mutation of state_ (HandleEvent(), Tick(),
  // RequestDispenseAction()/RequestStopAction(), HandleCommandSent(),
  // HandleCommandOutcome(), HandleApplianceState(), HandleTimeStatus(),
  // HandleConnectionLost()) runs on app::App::Run()'s own task, and
  // several of them update more than one field of state_ across
  // separate statements for a single logical transition (e.g.
  // HandleCommandOutcome()'s dispense_status/active_dispense_amount_ml/
  // delivered_ml trio). This reference is only ever a fully consistent
  // view of state_ when read from that same task, right after such a
  // transition has finished -- a caller on a *different* task copying
  // the referenced struct could observe a torn, in-between combination
  // of fields that never existed as a real state, not just a
  // stale-but-coherent one, if it happens to run while the app task is
  // mid-transition (FreeRTOS can preempt between any two statements
  // here). ui::UiManager::Render() is safe because it runs on this same
  // task, synchronously after each transition completes. Any other task
  // needing this data (M15's local HTTP API) must not call this
  // directly -- see app::App::Status()'s own comment for the cross-task
  // hand-off that makes that safe instead.
  [[nodiscard]] const dial_state::DialState& State() const { return state_; }

 private:
  dial_state::DialState state_{};
  DispenseSession dispense_session_;

  // M13.1: the encoder rotate step, in ml -- ApplySettings() overrides
  // this from persisted settings; HandleEvent()'s kRotateCw/kRotateCcw
  // cases read it instead of dial_state::kAmountStepMl directly. Starts
  // at that same compile-time constant, so behavior is unchanged until
  // ApplySettings() is ever called.
  int amount_step_ml_ = dial_state::kAmountStepMl;

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

  // M11: the two independent gate conditions GroheClient itself uses to
  // decide "ready for protocol writes" (see grohe_client.hpp's
  // ready_for_protocol_/subscribed_), observed here a second time purely
  // for display -- see dial_state.hpp's ConnectionStatus comment for why
  // this doesn't require changing GroheClient. Recomputed into
  // state_.connection_status by UpdateConnectionStatus() below whenever
  // either flag changes; both reset on HandleConnectionLost() so a
  // subsequent reconnect re-earns kReady from scratch.
  bool ble_ready_for_protocol_ = false;
  bool ble_subscribed_ = false;
  // Recomputes state_.connection_status from the two flags above; returns
  // whether it actually changed (so HandleReadyForProtocol()/
  // HandleSubscribed() don't force a redundant re-render when only one of
  // the two flags has flipped so far).
  bool UpdateConnectionStatus();

  // esp_timer_get_time() at the moment connection_status most recently
  // became kReady -- the baseline HandleTimeStatus() measures against to
  // decide "still syncing" vs. "no time" (see TimeStatus's own comment).
  int64_t connected_since_us_ = 0;

  // esp_timer_get_time() deadline for how long the Finished checkmark
  // (dial_state::DispenseStatus::kFinished) stays on screen before Tick()
  // returns to kIdle -- a separate, purely-cosmetic timer from
  // dispense_session_, which tracks the physical pour itself and is
  // already stopped by the time this one starts.
  int64_t finished_until_us_ = 0;

  // M13.6: same shape as finished_until_us_ above, for
  // dial_state::DispenseStatus::kFailed -- set by HandleCommandOutcome()
  // when a dispense request is rejected, read by Tick() to auto-return to
  // kIdle. dispense_session_ was never started for a rejected request, so
  // there is no physical timer to stop here, unlike kFinished.
  int64_t failed_until_us_ = 0;

  // M11.1: esp_timer_get_time() deadline for how long connection_status
  // stays kConnectionLost before Tick() flips it to kConnecting -- purely
  // cosmetic, same shape as finished_until_us_ above. (Re)set by every
  // HandleConnectionLost() call, including a retry-attempt failure that
  // arrives after the dial had already moved on to kConnecting -- see that
  // method's own comment.
  int64_t connection_lost_until_us_ = 0;
};

// Maps the UI-facing water type to the protocol's numeric taste value --
// the one place dial_state::WaterType and grohe_ble::WaterType meet,
// mirroring how HandleApplianceState() is the one place DialState and
// ApplianceState meet.
[[nodiscard]] grohe_ble::WaterType ToGroheWaterType(dial_state::WaterType type);

}  // namespace app
