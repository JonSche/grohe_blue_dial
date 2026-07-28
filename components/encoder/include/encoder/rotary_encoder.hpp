#pragma once

#include <atomic>
#include <cstdint>

#include "esp_attr.h"
#include "esp_err.h"

namespace encoder {

// Quadrature rotary encoder, decoded in software via GPIO edge interrupts.
// ESP32-C3 has no PCNT peripheral (SOC_PCNT_SUPPORTED is unset for this
// target -- see docs/ARCHITECTURE.md), so both phase pins are configured
// for GPIO_INTR_ANYEDGE and every edge is resolved directly in the ISR via
// a standard 4x4 quadrature transition table; no queue or dedicated task is
// needed.
class RotaryEncoder {
 public:
  RotaryEncoder() = default;
  ~RotaryEncoder();

  RotaryEncoder(const RotaryEncoder&) = delete;
  RotaryEncoder& operator=(const RotaryEncoder&) = delete;

  esp_err_t Init();

  // Signed step count since the last Reset() (or Init()).
  [[nodiscard]] int32_t GetPosition() const;
  void Reset();

 private:
  static void IRAM_ATTR OnGpioIsr(void* arg);

  std::atomic<int32_t> accumulated_count_{0};
  uint8_t last_state_ = 0;
  bool isr_installed_ = false;
};

}  // namespace encoder
