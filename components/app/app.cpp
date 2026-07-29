#include "app/app.hpp"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace app {
namespace {
constexpr char kTag[] = "app";
constexpr TickType_t kPollPeriod = pdMS_TO_TICKS(50);
}  // namespace

void App::Run() {
  ESP_LOGI(kTag, "Grohe Dial booting");

  ESP_ERROR_CHECK(display_.Init());

  if (display::Gc9a01Display::Lock()) {
    ui_.Init(display_.LvDisplay());
    display::Gc9a01Display::Unlock();
  } else {
    ESP_LOGE(kTag, "Failed to acquire LVGL lock -- UI was never built");
  }

  ESP_ERROR_CHECK(encoder_.Init());
  ESP_ERROR_CHECK(button_.Init());

  ESP_LOGI(kTag, "Startup complete");

  for (;;) {
    PollInputs();
    vTaskDelay(kPollPeriod);
  }
}

void App::PollInputs() {
  const int32_t position = encoder_.GetPosition();
  if (position != last_encoder_position_) {
    ESP_LOGI(kTag, "Encoder position: %ld", static_cast<long>(position));
    last_encoder_position_ = position;
  }

  const bool pressed = button_.IsPressed();
  if (pressed != last_button_pressed_) {
    ESP_LOGI(kTag, "Button %s", pressed ? "pressed" : "released");
    last_button_pressed_ = pressed;
  }
}

}  // namespace app
