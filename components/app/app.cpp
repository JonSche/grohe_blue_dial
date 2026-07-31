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
      const app::DialAction action = dial_controller_.HandleEvent(event);
      state_changed = true;
      switch (action) {
        case app::DialAction::kRequestDispense: {
          const auto& state = dial_controller_.State();
          const bool accepted = grohe_client_.RequestDispense(
              state.amount_ml, app::ToGroheWaterType(state.water_type));
          dial_controller_.HandleCommandSent(accepted);
          break;
        }
        case app::DialAction::kRequestStop: {
          const bool accepted = grohe_client_.RequestStop();
          dial_controller_.HandleCommandSent(accepted);
          break;
        }
        case app::DialAction::kNone:
          break;
      }
    });

    // Lifecycle events are still just logged here (unchanged since M3.1),
    // except for kConnectionFailed, which also forces DialController back
    // to Idle (M8's "disconnect during dispense" requirement).
    grohe_client_.Poll([this, &state_changed](const grohe_ble::BleEvent& event) {
      ESP_LOGI(kTag, "BLE event: %s (reason=%d)",
               grohe_ble::ToString(event.type), event.reason);
      if (event.type == grohe_ble::BleEventType::kConnectionFailed) {
        if (dial_controller_.HandleConnectionLost()) {
          state_changed = true;
        }
      }
    });
    if (dial_controller_.HandleApplianceState(
            grohe_client_.LatestApplianceState())) {
      state_changed = true;
    }
    if (dial_controller_.HandleCommandOutcome(
            grohe_client_.TakeCommandOutcome())) {
      state_changed = true;
    }
    if (dial_controller_.Tick()) {
      state_changed = true;
    }

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
