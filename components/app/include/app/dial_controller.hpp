#pragma once

#include "dial_state/dial_state.hpp"
#include "encoder/encoder_input.hpp"

namespace app {

// The application's interaction rules: owns the single DialState instance
// and is the only thing allowed to mutate it. Translates encoder events into
// state changes (amount stepping/clamping, water-type toggling, dispense
// requests). Contains no LVGL or UI code -- ui::UiManager only ever reads
// the resulting State() and renders it.
class DialController {
 public:
  void HandleEvent(encoder::EncoderEvent event);

  [[nodiscard]] const dial_state::DialState& State() const { return state_; }

 private:
  dial_state::DialState state_{};
};

}  // namespace app
