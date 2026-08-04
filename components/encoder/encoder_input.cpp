#include "encoder/encoder_input.hpp"

#include "esp_timer.h"

namespace encoder {
namespace {
// How long the button must be held before it counts as a long press rather
// than a short one. An input-timing parameter, not a business rule.
constexpr int64_t kLongPressThresholdUs = 600 * 1000;
}  // namespace

esp_err_t EncoderInput::Init() {
  esp_err_t err = encoder_.Init();
  if (err != ESP_OK) {
    return err;
  }
  return button_.Init();
}

void EncoderInput::Poll(const std::function<void(EncoderEvent)>& on_event) {
  // RotaryEncoder::GetPosition() increasing corresponds to physical
  // counter-clockwise rotation on this hardware (confirmed on-device), so
  // the CW/CCW event mapping is inverted relative to the raw position delta.
  const int32_t position = encoder_.GetPosition();
  while (position > last_position_) {
    on_event(EncoderEvent::kRotateCcw);
    ++last_position_;
  }
  while (position < last_position_) {
    on_event(EncoderEvent::kRotateCw);
    --last_position_;
  }

  const bool pressed = button_.IsPressed();
  const int64_t now_us = esp_timer_get_time();
  if (pressed && !button_was_pressed_) {
    // Just pressed: start timing this press.
    press_start_us_ = now_us;
    long_press_fired_ = false;
  } else if (pressed && !long_press_fired_ &&
             (now_us - press_start_us_) >= kLongPressThresholdUs) {
    on_event(EncoderEvent::kLongPress);
    long_press_fired_ = true;
  } else if (!pressed && button_was_pressed_ && !long_press_fired_) {
    // Released before crossing the long-press threshold.
    on_event(EncoderEvent::kShortPress);
  }
  button_was_pressed_ = pressed;
}

#ifdef CONFIG_GROHE_DEV_FEATURES
// Developer OTA validation hook only -- see the header's own comment.
bool EncoderInput::IsHeldFor(int64_t threshold_us) const {
  if (!button_was_pressed_) {
    return false;
  }
  return (esp_timer_get_time() - press_start_us_) >= threshold_us;
}
#endif  // CONFIG_GROHE_DEV_FEATURES

}  // namespace encoder
