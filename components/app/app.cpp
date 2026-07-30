#include "app/app.hpp"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace app {
namespace {
constexpr char kTag[] = "app";
// 20ms: tight enough that encoder rotation feels 1:1 with the physical
// motion (worst-case input latency ~20ms, well under human perceptible
// lag), without polling faster than the LVGL task's own 10ms render
// cadence + ~12ms SPI flush time can actually show -- going lower (e.g.
// 10ms) would just add CPU wake-ups for a delay difference nobody could
// see. Inherited unmeasured from M1's log-only poll loop at 50ms; retuned
// for M2 now that this loop drives real-time UI feedback.
constexpr TickType_t kPollPeriod = pdMS_TO_TICKS(20);
}  // namespace

void App::Run() {
  ESP_LOGI(kTag, "Grohe Dial booting");

  ESP_ERROR_CHECK(display_.Init());

  if (display::Gc9a01Display::Lock()) {
    ui_.Init(display_.LvDisplay());
    ui_.Render(dial_controller_.State());
    display::Gc9a01Display::Unlock();
  } else {
    ESP_LOGE(kTag, "Failed to acquire LVGL lock -- UI was never built");
  }

  ESP_ERROR_CHECK(encoder_input_.Init());

  // BLE is not allowed to take the rest of the firmware down with it: the
  // dial still has to work (display, encoder, UI) even if the radio never
  // comes up, so this is a log, not an ESP_ERROR_CHECK.
  if (grohe_client_.Init() != ESP_OK) {
    ESP_LOGE(kTag, "GroheClient::Init() failed -- continuing without BLE");
  }

  ESP_LOGI(kTag, "Startup complete");

  for (;;) {
    bool state_changed = false;
    encoder_input_.Poll([this, &state_changed](encoder::EncoderEvent event) {
      dial_controller_.HandleEvent(event);
      state_changed = true;
    });

    // Log-only: BLE has nothing to say about DialState yet (M4 discovers
    // the appliance; it does not connect to it) -- see
    // grohe_ble/grohe_client.hpp. M5 replaces this lambda's body with a
    // call into DialController, the same way the encoder callback above
    // already does; nothing about this loop's shape needs to change then.
    grohe_client_.Poll([](const grohe_ble::BleEvent& event) {
      ESP_LOGI(kTag, "BLE event: %s (reason=%d)",
               grohe_ble::ToString(event.type), event.reason);
    });

    if (state_changed) {
      if (display::Gc9a01Display::Lock()) {
        ui_.Render(dial_controller_.State());
        display::Gc9a01Display::Unlock();
      }
    }

    vTaskDelay(kPollPeriod);
  }
}

}  // namespace app
