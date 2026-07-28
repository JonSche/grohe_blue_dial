#include "encoder/button.hpp"

#include "board/board_config.hpp"
#include "driver/gpio.h"

namespace encoder {

esp_err_t Button::Init() {
  const gpio_config_t cfg = {
      .pin_bit_mask = 1ULL << board::kButtonPin,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  return gpio_config(&cfg);
}

bool Button::IsPressed() const {
  return gpio_get_level(board::kButtonPin) == 0;
}

}  // namespace encoder
