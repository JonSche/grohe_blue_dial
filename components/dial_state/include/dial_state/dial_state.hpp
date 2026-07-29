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

}  // namespace dial_state
