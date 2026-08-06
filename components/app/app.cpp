#include "app/app.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include "firmware_info/firmware_info.hpp"
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

// Display sleep: how long the dial waits with no activity (encoder input,
// or an active dispense -- see App::Run()'s activity computation) before
// turning the backlight off. Deliberately the only knob this feature has
// -- see SetBacklight()'s own call sites below for the intentionally
// minimal implementation this drives (backlight only; no LCD sleep
// command, no controller reset, no LVGL pause, no framebuffer change).
// Compile-time only, not a runtime setting, for now.
constexpr uint32_t kDisplaySleepTimeoutMs = 60000;

#ifdef CONFIG_GROHE_DEV_FEATURES
// ==== Developer OTA validation hook (M12.4) ====
// Gated behind CONFIG_GROHE_DEV_FEATURES (disabled by default -- see
// main/Kconfig.projbuild) precisely so it compiles out of, and costs
// nothing in, any build that doesn't explicitly ask for it. Reserved
// purely for developer testing: normal dial operation (dispense, stop,
// water type, display sleep, BLE) is completely unaffected either way.
//
// The one place the test OTA image URL is defined.
constexpr char kDeveloperOtaUrl[] =
    "https://github.com/JonSche/grohe_blue_dial/releases/download/"
    "v1.0.1-dev/grohe_dial.bin";
constexpr int64_t kDevOtaHoldThresholdUs = 5'000'000;  // ~5 s.
#endif  // CONFIG_GROHE_DEV_FEATURES
}  // namespace

void App::Run() {
  // M12.3: every build should be identifiable from its own boot log alone
  // -- see components/firmware_info/ for where each value actually comes
  // from (ESP-IDF's own esp_app_desc_t for Version()/BuildDate()/
  // BuildTime(), a build-time-generated header for the rest).
  ESP_LOGI(kTag, "Grohe Dial");
  ESP_LOGI(kTag, "Firmware: %s", firmware_info::Version());
  ESP_LOGI(kTag, "Commit: %s (%s%s)", firmware_info::GitCommit(),
           firmware_info::GitBranch(), firmware_info::GitDirty() ? ", dirty" : "");
  ESP_LOGI(kTag, "Built: %s %s", firmware_info::BuildDate(), firmware_info::BuildTime());

  // M12.4: confirms this boot is healthy (cancelling any pending OTA
  // rollback) before anything else runs -- see OtaManager::Init()'s own
  // comment. Cheap and always safe to call; not gated on any of the
  // subsystems below.
  ota_.Init();

  // M12.5: one-time Wi-Fi driver/event-loop setup (NVS, netif, the
  // default event loop, this class's own event handlers) -- does not
  // connect yet. Must happen before grohe_client_.Init() below, which
  // (via its own SntpTimeProvider) is the first thing to actually
  // acquire a connection through it; ota_'s later CheckForUpdate()/
  // StartUpdate() calls share this exact same instance rather than
  // bringing up a second, independent Wi-Fi session -- see
  // wifi_connection.hpp's own comment.
  ESP_ERROR_CHECK(wifi_connection_.Init());

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

  // Display sleep (backlight only -- see kDisplaySleepTimeoutMs's own
  // comment): last_activity_us resets on any encoder event, and is held
  // continuously refreshed for as long as the dispense interaction is
  // still in progress from the user's perspective -- dispense_status ==
  // kDispensing or kStopping (see the activity computation below). The
  // display must never sleep mid-pour or mid-stop, however long either
  // takes. kFinished/kIdle are not activity by themselves: the timeout
  // only resumes once the dial is back to its normal idle state.
  int64_t last_activity_us = esp_timer_get_time();
  bool backlight_on = true;

#ifdef CONFIG_GROHE_DEV_FEATURES
  // See kDeveloperOtaUrl's own comment. Fires the check/update once per
  // continuous hold past the threshold (mirroring EncoderEvent::
  // kLongPress's own "fire once, not repeatedly" behaviour), resetting
  // the moment the button is released.
  bool dev_ota_hold_fired = false;
#endif  // CONFIG_GROHE_DEV_FEATURES

  for (;;) {
    bool state_changed = false;
    bool encoder_activity = false;
    encoder_input_.Poll([this, &state_changed,
                         &encoder_activity](encoder::EncoderEvent event) {
      encoder_activity = true;
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
    // except for kConnectionFailed (M8's "disconnect during dispense"
    // requirement) and, as of M11, kReadyForProtocol/kSubscribed -- the
    // same two events grohe_client_ itself gates protocol writes on,
    // observed here a second time purely to drive the UI's
    // connection_status (see dial_state.hpp's ConnectionStatus comment for
    // why this doesn't require changing GroheClient).
    grohe_client_.Poll([this, &state_changed](const grohe_ble::BleEvent& event) {
      ESP_LOGI(kTag, "BLE event: %s (reason=%d)",
               grohe_ble::ToString(event.type), event.reason);
      switch (event.type) {
        case grohe_ble::BleEventType::kConnectionFailed:
          if (dial_controller_.HandleConnectionLost()) {
            state_changed = true;
          }
          break;
        case grohe_ble::BleEventType::kReadyForProtocol:
          if (dial_controller_.HandleReadyForProtocol()) {
            state_changed = true;
          }
          break;
        case grohe_ble::BleEventType::kSubscribed:
          if (dial_controller_.HandleSubscribed()) {
            state_changed = true;
          }
          break;
        case grohe_ble::BleEventType::kHostSynced:
        case grohe_ble::BleEventType::kHostReset:
        case grohe_ble::BleEventType::kDeviceFound:
          break;
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
    if (dial_controller_.HandleTimeStatus(grohe_client_.HasValidTime())) {
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

    // Display sleep: wake immediately on encoder activity, sleep after
    // kDisplaySleepTimeoutMs with none -- except while a dispense
    // interaction is still in progress from the user's perspective
    // (kDispensing or kStopping), which counts as continuous activity so
    // the display can never time out mid-pour or mid-stop. Backlight only
    // -- see kDisplaySleepTimeoutMs's own comment for everything this
    // deliberately doesn't touch.
    const dial_state::DispenseStatus dispense_status =
        dial_controller_.State().dispense_status;
    const bool keep_display_awake =
        dispense_status == dial_state::DispenseStatus::kDispensing ||
        dispense_status == dial_state::DispenseStatus::kStopping;
    const int64_t now_us = esp_timer_get_time();
    if (encoder_activity || keep_display_awake) {
      last_activity_us = now_us;
      if (!backlight_on) {
        display_.SetBacklight(true);
        backlight_on = true;
      }
    } else if (backlight_on &&
               now_us - last_activity_us >=
                   static_cast<int64_t>(kDisplaySleepTimeoutMs) * 1000) {
      display_.SetBacklight(false);
      backlight_on = false;
    }

#ifdef CONFIG_GROHE_DEV_FEATURES
    // Developer OTA validation hook -- see main/Kconfig.projbuild's own
    // help text. Only reachable by deliberately holding the encoder
    // button for ~5 s while idle; does nothing on every other input, and
    // never runs automatically. Entirely absent from a default build.
    if (encoder_input_.IsHeldFor(kDevOtaHoldThresholdUs)) {
      if (!dev_ota_hold_fired &&
          dispense_status == dial_state::DispenseStatus::kIdle) {
        dev_ota_hold_fired = true;
        ESP_LOGW(kTag, "DEV HOOK: manual OTA check requested (5s encoder hold)");
        const ota::UpdateCheckResult check = ota_.CheckForUpdate(kDeveloperOtaUrl);
        ESP_LOGW(kTag,
                 "DEV HOOK: CheckForUpdate -> update_available=%d "
                 "remote_version=\"%s\" last_error=%s",
                 check.update_available, check.remote_version,
                 esp_err_to_name(ota_.LastError()));
        if (check.update_available) {
          ESP_LOGW(kTag, "DEV HOOK: update available -- starting StartUpdate()");
          const esp_err_t start_err = ota_.StartUpdate(kDeveloperOtaUrl);
          // Only reached on failure -- StartUpdate() calls esp_restart()
          // itself on success and never returns here.
          ESP_LOGE(kTag, "DEV HOOK: StartUpdate failed: %s",
                   esp_err_to_name(start_err));
        } else {
          ESP_LOGW(kTag, "DEV HOOK: no update available (or check failed) -- not starting");
        }
      }
    } else {
      dev_ota_hold_fired = false;
    }
#endif  // CONFIG_GROHE_DEV_FEATURES

    vTaskDelay(kPollPeriod);
  }
}

}  // namespace app
