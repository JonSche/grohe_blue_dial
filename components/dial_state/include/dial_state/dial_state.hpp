#pragma once

namespace dial_state {

enum class WaterType {
  kStill,
  kSparkling,
};

// Single source of truth for the dial's UI. app::DialController owns and
// mutates the one instance of this; ui::UiManager only ever reads it via
// Render(). No component may modify LVGL objects and this struct
// independently -- the display must always be a pure function of this data.
struct DialState {
  int amount_ml = 500;
  WaterType water_type = WaterType::kSparkling;

  // M7: the appliance's one confirmed protocol response (see
  // grohe_ble::ApplianceState, translated by app::DialController --
  // dial_state has no dependency on grohe_ble, matching its own
  // CMakeLists.txt comment). False until any response has been decoded,
  // i.e. still connecting/subscribing, or before the stop() probe's
  // acknowledgement arrives.
  bool appliance_response_received = false;
  bool appliance_response_success = false;
  int appliance_response_code = 0;  // Meaningful only if received above.
};

inline constexpr int kMinAmountMl = 100;
inline constexpr int kMaxAmountMl = 2000;
inline constexpr int kAmountStepMl = 100;

inline constexpr const char* WaterTypeLabel(WaterType type) {
  switch (type) {
    case WaterType::kStill:
      return "STILL";
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
