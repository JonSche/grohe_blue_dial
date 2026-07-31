#include "app/dial_controller.hpp"

#include <algorithm>

#include "esp_log.h"
#include "esp_timer.h"
#include "grohe_ble/grohe_client.hpp"
#include "grohe_ble/grohe_protocol.hpp"

// grohe_ble headers transitively pull in nimble/ble.h -> os/os.h, which
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

DialAction DialController::HandleEvent(encoder::EncoderEvent event) {
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
      if (command_pending_) {
        break;  // Debounce -- a previous request hasn't resolved yet.
      }
      if (state_.dispense_status == dial_state::DispenseStatus::kIdle) {
        ESP_LOGI(kTag, "Dispense requested: %d ml", state_.amount_ml);
        pending_dispense_amount_ml_ = state_.amount_ml;
        return DialAction::kRequestDispense;
      }
      ESP_LOGI(kTag, "Stop requested");
      return DialAction::kRequestStop;
    case EncoderEvent::kLongPress:
      if (state_.dispense_status == dial_state::DispenseStatus::kIdle) {
        state_.water_type =
            (state_.water_type == dial_state::WaterType::kStill)
                ? dial_state::WaterType::kSparkling
                : dial_state::WaterType::kStill;
      }
      break;
  }
  return DialAction::kNone;
}

void DialController::HandleCommandSent(bool accepted) {
  command_pending_ = accepted;
}

bool DialController::HandleCommandOutcome(
    const grohe_ble::CommandOutcome& outcome) {
  if (!outcome.available) {
    return false;
  }
  command_pending_ = false;

  if (!outcome.state.is_success) {
    // Gracefully handled, not invented: the command wasn't actually
    // accepted, so dispense_status stays exactly as it was (still Idle if
    // this was a rejected dispense request; still Dispensing, relying on
    // the timer, if this was a rejected stop request).
    ESP_LOGW(kTag, "%s command rejected: code=%ld",
             outcome.was_dispense ? "dispense" : "stop",
             outcome.state.response_code);
    return false;
  }

  if (outcome.was_dispense) {
    state_.dispense_status = dial_state::DispenseStatus::kDispensing;
    dispense_session_.Start(pending_dispense_amount_ml_, esp_timer_get_time());
  } else {
    state_.dispense_status = dial_state::DispenseStatus::kIdle;
    dispense_session_.Stop();
  }
  return true;
}

bool DialController::Tick() {
  if (state_.dispense_status != dial_state::DispenseStatus::kDispensing) {
    return false;
  }
  if (!dispense_session_.Finished(esp_timer_get_time())) {
    return false;
  }
  dispense_session_.Stop();
  state_.dispense_status = dial_state::DispenseStatus::kIdle;
  return true;
}

bool DialController::HandleConnectionLost() {
  command_pending_ = false;
  dispense_session_.Stop();
  if (state_.dispense_status == dial_state::DispenseStatus::kIdle) {
    return false;
  }
  state_.dispense_status = dial_state::DispenseStatus::kIdle;
  return true;
}

bool DialController::HandleApplianceState(
    const grohe_ble::ApplianceState& appliance_state) {
  if (!appliance_state.received ||
      appliance_state.sequence == last_seen_sequence_) {
    return false;
  }
  last_seen_sequence_ = appliance_state.sequence;
  state_.appliance_response_received = true;
  state_.appliance_response_success = appliance_state.is_success;
  state_.appliance_response_code =
      static_cast<int>(appliance_state.response_code);
  return true;
}

bool DialController::HandleTimeStatus(bool available) {
  if (state_.time_available == available) {
    return false;
  }
  state_.time_available = available;
  return true;
}

grohe_ble::WaterType ToGroheWaterType(dial_state::WaterType type) {
  switch (type) {
    case dial_state::WaterType::kStill:
      return grohe_ble::WaterType::kStill;
    case dial_state::WaterType::kSparkling:
      return grohe_ble::WaterType::kSparkling;
  }
  return grohe_ble::WaterType::kUnknown;
}

}  // namespace app
