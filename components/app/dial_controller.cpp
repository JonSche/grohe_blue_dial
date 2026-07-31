#include "app/dial_controller.hpp"

#include <algorithm>

#include "esp_log.h"
#include "grohe_ble/grohe_protocol.hpp"

// grohe_protocol.hpp transitively pulls in nimble/ble.h -> os/os.h, which
// #defines min/max as macros -- undone immediately so std::min/std::max
// below (in HandleEvent(), unrelated to BLE) keep meaning the standard
// library functions, not this macro pair.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace app {
namespace {
constexpr char kTag[] = "dial_controller";
}  // namespace

void DialController::HandleEvent(encoder::EncoderEvent event) {
  using encoder::EncoderEvent;
  switch (event) {
    case EncoderEvent::kRotateCw:
      state_.amount_ml = std::min(state_.amount_ml + dial_state::kAmountStepMl,
                                   dial_state::kMaxAmountMl);
      break;
    case EncoderEvent::kRotateCcw:
      state_.amount_ml = std::max(state_.amount_ml - dial_state::kAmountStepMl,
                                   dial_state::kMinAmountMl);
      break;
    case EncoderEvent::kShortPress:
      ESP_LOGI(kTag, "Dispense requested: %d ml", state_.amount_ml);
      break;
    case EncoderEvent::kLongPress:
      state_.water_type = (state_.water_type == dial_state::WaterType::kStill)
                               ? dial_state::WaterType::kSparkling
                               : dial_state::WaterType::kStill;
      break;
  }
}

bool DialController::HandleApplianceState(
    const grohe_ble::ApplianceState& appliance_state) {
  if (!appliance_state.received) {
    return false;  // Nothing decoded yet; state_ already reflects that.
  }
  const bool changed =
      !state_.appliance_response_received ||
      state_.appliance_response_success != appliance_state.is_success ||
      state_.appliance_response_code != appliance_state.response_code;
  state_.appliance_response_received = true;
  state_.appliance_response_success = appliance_state.is_success;
  state_.appliance_response_code = static_cast<int>(appliance_state.response_code);
  return changed;
}

}  // namespace app
