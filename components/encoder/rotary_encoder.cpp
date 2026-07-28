#include "encoder/rotary_encoder.hpp"

#include "board/board_config.hpp"
#include "driver/gpio.h"
#include "esp_log.h"

namespace encoder {
namespace {
constexpr char kTag[] = "rotary_encoder";

// Standard quadrature edge-transition table. Index = (previous 2-bit state
// << 2) | current 2-bit state, where each state packs phase A into bit 1
// and phase B into bit 0. Invalid/bounce transitions (both bits changing
// between reads) resolve to 0.
constexpr int8_t kTransitionTable[16] = {
    0, -1, 1,  0,
    1,  0, 0, -1,
   -1,  0, 0,  1,
    0,  1, -1, 0,
};

// Called from ISR context (via OnGpioIsr) and once from Init(); marked
// IRAM_ATTR unconditionally so it's always safe regardless of inlining.
uint8_t IRAM_ATTR ReadState() {
  return (static_cast<uint8_t>(gpio_get_level(board::kEncoderPinPhaseA)) << 1) |
         static_cast<uint8_t>(gpio_get_level(board::kEncoderPinPhaseB));
}
}  // namespace

RotaryEncoder::~RotaryEncoder() {
  if (isr_installed_) {
    gpio_isr_handler_remove(board::kEncoderPinPhaseA);
    gpio_isr_handler_remove(board::kEncoderPinPhaseB);
  }
}

esp_err_t RotaryEncoder::Init() {
  ESP_LOGI(kTag,
           "Configuring GPIO-ISR quadrature decode (no PCNT on this target)");

  const gpio_config_t phase_cfg = {
      .pin_bit_mask = (1ULL << board::kEncoderPinPhaseA) |
                      (1ULL << board::kEncoderPinPhaseB),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_ANYEDGE,
  };
  ESP_ERROR_CHECK(gpio_config(&phase_cfg));

  last_state_ = ReadState();

  // gpio_install_isr_service is process-wide; tolerate it already being
  // installed by another component instead of failing here.
  const esp_err_t isr_service_err = gpio_install_isr_service(0);
  if (isr_service_err != ESP_OK && isr_service_err != ESP_ERR_INVALID_STATE) {
    return isr_service_err;
  }

  ESP_ERROR_CHECK(
      gpio_isr_handler_add(board::kEncoderPinPhaseA, OnGpioIsr, this));
  ESP_ERROR_CHECK(
      gpio_isr_handler_add(board::kEncoderPinPhaseB, OnGpioIsr, this));
  isr_installed_ = true;

  return ESP_OK;
}

int32_t RotaryEncoder::GetPosition() const {
  return accumulated_count_.load(std::memory_order_relaxed);
}

void RotaryEncoder::Reset() {
  accumulated_count_.store(0, std::memory_order_relaxed);
}

void IRAM_ATTR RotaryEncoder::OnGpioIsr(void* arg) {
  auto* self = static_cast<RotaryEncoder*>(arg);
  const uint8_t new_state = ReadState();
  const uint8_t index = static_cast<uint8_t>((self->last_state_ << 2) | new_state);
  self->last_state_ = new_state;
  self->accumulated_count_.fetch_add(kTransitionTable[index],
                                      std::memory_order_relaxed);
}

}  // namespace encoder
