#pragma once

#include "esp_err.h"

namespace encoder {

// Momentary push button wired active-low to ground, with the internal
// pull-up enabled. Polled rather than interrupt-driven; simple debouncing is
// left to the caller (e.g. requiring N consecutive polls of the same state).
class Button {
 public:
  Button() = default;

  esp_err_t Init();
  [[nodiscard]] bool IsPressed() const;
};

}  // namespace encoder
