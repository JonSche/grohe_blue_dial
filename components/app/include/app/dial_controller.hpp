#pragma once

#include "dial_state/dial_state.hpp"
#include "encoder/encoder_input.hpp"

// Forward-declared rather than #include "grohe_ble/grohe_protocol.hpp":
// that header transitively pulls in ble_manager.hpp -> nimble/ble.h ->
// os/os.h, which #defines min/max as macros -- fine for grohe_ble's own
// files, but this header is included by dial_controller.cpp, which calls
// std::min/std::max. HandleApplianceState() only needs ApplianceState by
// const reference, so a forward declaration is enough here; the .cpp
// includes the real header where it accesses the struct's fields.
namespace grohe_ble {
struct ApplianceState;
}  // namespace grohe_ble

namespace app {

// The application's interaction rules: owns the single DialState instance
// and is the only thing allowed to mutate it. Translates encoder events into
// state changes (amount stepping/clamping, water-type toggling, dispense
// requests). Contains no LVGL or UI code -- ui::UiManager only ever reads
// the resulting State() and renders it.
class DialController {
 public:
  void HandleEvent(encoder::EncoderEvent event);

  // M7: translates the appliance's decoded protocol response into
  // DialState's own plain fields -- dial_state has no dependency on
  // grohe_ble (see dial_state.hpp), so this is the one place that
  // bridges the two, the same role HandleEvent() already plays for
  // encoder::EncoderEvent. Returns true if anything actually changed, so
  // App::Run() only re-renders when it needs to.
  bool HandleApplianceState(const grohe_ble::ApplianceState& appliance_state);

  [[nodiscard]] const dial_state::DialState& State() const { return state_; }

 private:
  dial_state::DialState state_{};
};

}  // namespace app
