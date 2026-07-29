#include "app/dial_controller.hpp"

#include <algorithm>

#include "esp_log.h"

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

}  // namespace app
