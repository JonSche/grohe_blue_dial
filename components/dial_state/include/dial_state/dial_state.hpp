#pragma once

namespace dial_state {

// Ordered to match the long-press cycle (M10): Still -> Medium -> Sparkling
// -> Still -- see app::DialController::HandleEvent's kLongPress case.
enum class WaterType {
  kStill,
  kMedium,
  kSparkling,
};

// Whether the dial is currently waiting out a dispense (M8), stopping it
// early, or briefly showing the completion checkmark (M11). Purely a
// rendering concern -- the actual timing model and state transitions live
// in app::DialController/app::DispenseSession, not here (see
// dial_state's own CMakeLists.txt comment: no grohe_ble dependency).
//
// kStopping: a stop request has been sent but not yet acknowledged --
// distinct from kDispensing so the UI can show "STOPPING..." instead of
// "PRESS TO STOP" for that window (see docs/ui/dispense_animation_mockups.md,
// "Stop and Finished"). kFinished: the predicted duration has elapsed and
// the checkmark hold is in progress before returning to kIdle.
enum class DispenseStatus {
  kIdle,
  kDispensing,
  kStopping,
  kFinished,
};

// BLE connection readiness, independent of whether a valid time is
// available (see TimeStatus below) -- the two are separate subsystems in
// the real firmware (SNTP vs. the BLE link) and must never be conflated in
// the UI (see docs/ui/dispense_animation_mockups.md's state machine and
// connectivity-indicator sections). Tracked by app::DialController from the
// same BleEventType::kReadyForProtocol/kSubscribed/kConnectionFailed events
// grohe_ble::GroheClient itself gates on -- see dial_controller.cpp for why
// this is a second, independent observation rather than a change to
// GroheClient.
enum class ConnectionStatus {
  kConnecting,
  kReady,
  kConnectionLost,
};

// Whether a valid Unix epoch is available, and if not, whether the dial is
// still within the expected Wi-Fi/SNTP window or has been waiting long
// enough that something is likely wrong ("Time Sync" vs. "No Time" in the
// frozen UI spec). The threshold is a UI-only judgement call -- TimeProvider
// itself has no notion of "given up" -- see dial_controller.cpp's
// kTimeSyncTimeoutUs for the exact value and reasoning.
enum class TimeStatus {
  kSyncing,
  kAvailable,
  kUnavailable,
};

// Single source of truth for the dial's UI. app::DialController owns and
// mutates the one instance of this; ui::UiManager only ever reads it via
// Render(). No component may modify LVGL objects and this struct
// independently -- the display must always be a pure function of this data.
struct DialState {
  int amount_ml = 500;
  WaterType water_type = WaterType::kSparkling;
  DispenseStatus dispense_status = DispenseStatus::kIdle;

  // The amount actually committed to the in-progress dispense (M11) --
  // frozen at the moment a dispense starts, and deliberately independent
  // of amount_ml above, which keeps changing live as the encoder is
  // rotated (rotation is never blocked; see DialController::HandleEvent's
  // own comment -- it only affects the *next* dispense). ui::UiManager
  // reads this instead of amount_ml whenever dispense_status != kIdle, so
  // the ring can hold the one invariant the frozen UI spec requires: it
  // represents the amount the *current* pour was started with, and never
  // changes meaning mid-pour just because the dial was rotated in the
  // meantime. Meaningless (0) while kIdle.
  int active_dispense_amount_ml = 0;

  // Delivered volume so far, rounded to the nearest 10 ml (matching the
  // frozen UI spec's count-up cadence) -- computed by DialController::
  // Tick() from DispenseSession, since ui::UiManager has no access to
  // timing data of its own (Render() is a pure function of this struct).
  // Meaningful only while kDispensing or kStopping (frozen at its last
  // value while stopping); 0 otherwise.
  int delivered_ml = 0;

  // The appliance's latest confirmed protocol response (see
  // grohe_ble::ApplianceState, translated by app::DialController --
  // dial_state has no dependency on grohe_ble, matching its own
  // CMakeLists.txt comment). False until any response has been decoded,
  // i.e. still connecting/subscribing, or before the first command's
  // acknowledgement arrives. Purely diagnostic (see
  // DialController::HandleApplianceState's own comment) -- independent of
  // connection_status/time_status below, which gate whether the dial is
  // usable at all.
  bool appliance_response_received = false;
  bool appliance_response_success = false;
  int appliance_response_code = 0;  // Meaningful only if received above.

  // BLE readiness and time availability (M11) -- see ConnectionStatus's
  // and TimeStatus's own comments. Deliberately two separate fields, never
  // combined into one: the frozen UI spec requires that losing the BLE
  // link never visibly affects the time indicator, and a delayed time
  // sync never visibly affects the connection indicator, because they
  // really are independent subsystems in the firmware underneath.
  ConnectionStatus connection_status = ConnectionStatus::kConnecting;
  TimeStatus time_status = TimeStatus::kSyncing;
};

inline constexpr int kMinAmountMl = 100;
inline constexpr int kMaxAmountMl = 2000;
inline constexpr int kAmountStepMl = 100;

inline constexpr const char* WaterTypeLabel(WaterType type) {
  switch (type) {
    case WaterType::kStill:
      return "STILL";
    case WaterType::kMedium:
      return "MEDIUM";
    case WaterType::kSparkling:
      return "SPARKLING";
  }
  return "";
}

// Mirrors the Python reference's ResponseCode names (see
// grohe_ble::ApplianceState's own source comment) for display -- returns
// nullptr for a code this milestone doesn't have a name for, so the caller
// can fall back to showing the raw number instead of guessing a label.
inline constexpr const char* ResponseCodeName(int code) {
  switch (code) {
    case 0:
      return "SUCCESS";
    case 1:
      return "INVALID_HMAC";
    case 2:
      return "TIMESTAMP_EXPIRED";
    case 3:
      return "GUEST_MODE_DISABLED";
    case 4:
      return "APPLIANCE_INTERNAL_ERROR";
    default:
      return nullptr;
  }
}

}  // namespace dial_state
