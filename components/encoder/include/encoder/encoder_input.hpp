#pragma once

#include <cstdint>
#include <functional>

#include "encoder/button.hpp"
#include "encoder/rotary_encoder.hpp"
#include "esp_err.h"

namespace encoder {

// Strongly-typed input events, decoded from the raw rotary encoder position
// and button level. No knowledge of LVGL, application state, or what these
// events mean to the app -- purely a hardware-input abstraction.
enum class EncoderEvent {
  kRotateCw,
  kRotateCcw,
  kShortPress,
  kLongPress,
};

// Owns the raw RotaryEncoder + Button and converts their state into
// EncoderEvents. Poll() must be called periodically (e.g. from the app's
// main loop); it invokes on_event once per event detected since the last
// call -- zero, one, or several times per call.
class EncoderInput {
 public:
  EncoderInput() = default;

  esp_err_t Init();

  void Poll(const std::function<void(EncoderEvent)>& on_event);

 private:
  RotaryEncoder encoder_;
  Button button_;

  int32_t last_position_ = 0;
  bool button_was_pressed_ = false;
  int64_t press_start_us_ = 0;
  bool long_press_fired_ = false;
};

}  // namespace encoder
