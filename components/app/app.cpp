#include "app/app.hpp"

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "firmware_info/firmware_info.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mem_diag/mem_diag.hpp"
#include "ota/ota_rollback.hpp"

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

// M15: bounded wait for the API command/result queue round-trip (see
// App::RequestDispense()/RequestStop() and the app_command_queue_ drain
// in Run()'s loop below). Generous relative to the 20ms loop cadence
// (kPollPeriod) a response should normally arrive within -- a timeout
// this large firing would itself be the anomaly worth logging, not a
// tuning knob for normal operation.
constexpr TickType_t kApiQueueTimeout = pdMS_TO_TICKS(1000);
}  // namespace

void App::Run() {
  // TEMPORARY DIAGNOSTIC (whole-system RAM investigation) -- the earliest
  // possible checkpoint, before this function does anything else.
  mem_diag::Log(kTag, "BOOT");

  // M15: created before anything below can possibly start the HTTP
  // server that would call RequestDispense()/RequestStop() (i.e. before
  // provisioning_server_.Init(), several steps down) -- see api_command_
  // queue_/api_result_queue_'s own comment on app.hpp for the full
  // mechanism these back.
  api_command_queue_ = xQueueCreate(1, sizeof(ApiCommand));
  api_result_queue_ = xQueueCreate(1, sizeof(dial_api::RequestResult));
  api_status_queue_ = xQueueCreate(1, sizeof(dial_state::DialState));

  // M12.3: every build should be identifiable from its own boot log alone
  // -- see components/firmware_info/ for where each value actually comes
  // from (ESP-IDF's own esp_app_desc_t for Version()/BuildDate()/
  // BuildTime(), a build-time-generated header for the rest).
  ESP_LOGI(kTag, "Grohe Dial");
  ESP_LOGI(kTag, "Firmware: %s", firmware_info::Version());
  ESP_LOGI(kTag, "Commit: %s (%s%s)", firmware_info::GitCommit(),
           firmware_info::GitBranch(), firmware_info::GitDirty() ? ", dirty" : "");
  ESP_LOGI(kTag, "Built: %s %s", firmware_info::BuildDate(), firmware_info::BuildTime());

  // M13.1: local, NVS-backed, no Wi-Fi/networking involved -- loads before
  // anything else so the very first UI render (below) already reflects
  // any stored settings rather than dial_state.hpp's compile-time ones
  // getting shown and then silently replaced. Never fails hard; see
  // DialSettingsStore::Init()'s own comment.
  dial_settings_.Init();
  dial_controller_.ApplySettings(dial_settings_.Values());

  // One-time Wi-Fi driver/event-loop setup (NVS, netif, the default event
  // loop, this class's own event handlers) -- does not connect yet. Must
  // happen before grohe_client_.Init() below, which (via its own
  // SntpTimeProvider) is the first thing to actually acquire a connection
  // through it -- see wifi_connection.hpp's own comment.
  ESP_ERROR_CHECK(wifi_connection_.Init());

  // TEMPORARY DIAGNOSTIC (whole-system RAM investigation): isolates
  // wifi_connection_.Init()'s own cost (esp_netif_init(), the default
  // event loop, esp_wifi_init()'s static RX/TX buffer pools -- all
  // committed here, before any actual Wi-Fi connection attempt) from
  // everything that runs after it (display/LVGL, BLE/NimBLE) -- see the
  // existing BOOT/BLE_INITIALIZED checkpoints this sits between.
  mem_diag::Log(kTag, "WIFI_INITIALIZED");

  // M12: non-blocking -- registers a permanent Wi-Fi acquisition and
  // starts the OTA HTTP server once it comes up (or logs and does
  // nothing if no OTA secret is configured). Never gates the rest of
  // this startup sequence -- see ota_server.hpp's own comment.
  ota_server_.Init();

  ESP_ERROR_CHECK(display_.Init());

  if (display::Gc9a01Display::Lock()) {
    ui_.Init(display_.LvDisplay());
    ui_.Render(dial_controller_.State());
    display::Gc9a01Display::Unlock();
  } else {
    ESP_LOGE(kTag, "Failed to acquire LVGL lock -- UI was never built");
  }

  ESP_ERROR_CHECK(encoder_input_.Init());

  // M13.1: local, NVS-backed -- loads any previously provisioned Grohe
  // credentials (falling back to the gitignored-local-header developer
  // ones if none are stored) before grohe_client_.Init() ever has a
  // chance to send a command using them. Never fails hard; see
  // NvsCredentialsProvider::Init()'s own comment.
  grohe_credentials_provider_.Init();

  // M13.2: non-blocking, same shape as ota_server_.Init() above --
  // registers a permanent Wi-Fi acquisition and starts the provisioning
  // HTTP server once it comes up (or logs and does nothing if no
  // provisioning secret is configured). Placed after
  // grohe_credentials_provider_.Init() (just above), not next to
  // ota_server_.Init(), purely so a reader never has to wonder whether
  // the credentials provider a provisioning request would update is
  // already loaded by the time this starts accepting connections --
  // functionally the two orderings are equivalent, since no real
  // request can arrive before Wi-Fi itself finishes associating anyway.
  provisioning_server_.Init();

  // TEMPORARY DIAGNOSTIC (whole-system RAM investigation): the cleanest
  // available boundary immediately before BLE/NimBLE init -- everything
  // above (display/LVGL, encoder, credentials, provisioning
  // registration) is already committed by this point, and nothing below
  // runs before grohe_client_.Init() itself. Note this call also runs
  // SntpTimeProvider::Init() first (a small WifiConnection::AcquireAsync()
  // registration, not a BLE cost) before BleManager::Init() -- see
  // GroheClient::Init()'s own body -- so the BLE_PRE_INIT ->
  // BLE_INITIALIZED delta is "BLE plus that one small registration", not
  // 100% exclusively BLE; not split further here since doing so would
  // require a change inside components/grohe_ble/ itself, out of scope
  // for this diagnostic pass.
  mem_diag::Log(kTag, "BLE_PRE_INIT");

  // BLE is not allowed to take the rest of the firmware down with it: the
  // dial still has to work (display, encoder, UI) even if the radio never
  // comes up, so this is a log, not an ESP_ERROR_CHECK.
  if (grohe_client_.Init() != ESP_OK) {
    ESP_LOGE(kTag, "GroheClient::Init() failed -- continuing without BLE");
  }

  // M12: only now, after every subsystem above has finished initializing
  // -- not at the top of Run() -- so a crash during startup itself still
  // leaves this boot's OTA image unconfirmed and correctly triggers the
  // bootloader's automatic rollback (see ota_rollback.hpp). A no-op
  // outside a pending-verify boot (i.e. every normal boot).
  ota::ConfirmBootValid();

  // M16.3: registers this task (the one running Run()'s own loop below
  // -- ESP-IDF's "main" task, per app_main()'s own single call site)
  // with the Task Watchdog Timer (TWDT) -- nullptr means "the calling
  // task". Deliberately placed *here*, after boot/init has already
  // finished, not at the top of Run(): the init sequence above has its
  // own several-second, legitimately variable-length waits (Wi-Fi
  // association, BLE controller bring-up, ...) that were never
  // instrumented with their own reset calls -- registering earlier
  // without also touching every one of those steps would risk a false
  // trip on a slow-but-healthy boot, precisely the regression this
  // milestone's own requirement ("Normalbetrieb darf nicht
  // beeinträchtigt werden") warns against. This covers the steady-state
  // loop specifically -- the failure mode actually being hardened
  // against (a future bug hanging the app task during normal
  // operation), not the boot sequence, which already has the
  // bootloader's own OTA-rollback safety net (ConfirmBootValid() above)
  // for a crash, just not yet for a hang with no reset at all -- a
  // separate, larger undertaking, not in scope here.
  //
  // kApiQueueTimeout (1s, app.cpp's own anonymous-namespace constant)
  // is NOT a concern here: that bounded wait belongs to the *httpd*
  // task (inside RequestDispense()/RequestStop()/Status(), all called
  // *from* the httpd task, never run on this one) -- this task's own
  // loop never blocks anywhere near CONFIG_ESP_TASK_WDT_TIMEOUT_S (5s,
  // see sdkconfig.defaults's own comment), only the ordinary 20ms
  // vTaskDelay(kPollPeriod) at the bottom of every iteration.
  if (const esp_err_t err = esp_task_wdt_add(nullptr); err != ESP_OK) {
    ESP_LOGE(kTag, "esp_task_wdt_add failed: %s -- app task hangs will "
             "no longer trigger an automatic reset", esp_err_to_name(err));
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

  // TEMPORARY DIAGNOSTIC (whole-system RAM investigation): a one-shot
  // task-stack survey, deliberately delayed past boot (see
  // kTaskStackSurveyDelayUs below) so Wi-Fi/BLE/OTA have all had a
  // real chance to create their own tasks first -- a survey run at t=0
  // would only ever find "main". Not gated on any of those actually
  // having succeeded; mem_diag::LogTaskStacks() itself reports each
  // candidate task as found or not found, never guesses.
  constexpr int64_t kTaskStackSurveyDelayUs = 15'000'000;  // 15s
  bool logged_task_stacks = false;
  const int64_t boot_time_us = esp_timer_get_time();

  for (;;) {
    // M16.3: fed unconditionally, first thing every iteration -- as
    // long as this loop keeps cycling at its normal ~20ms cadence
    // (kPollPeriod), this never comes remotely close to the 5s TWDT
    // timeout. A future bug that genuinely hangs this task (blocks
    // without ever reaching the next iteration) stops feeding it and
    // triggers a panic-reset instead of hanging forever unnoticed.
    esp_task_wdt_reset();

    if (!logged_task_stacks &&
        esp_timer_get_time() - boot_time_us >= kTaskStackSurveyDelayUs) {
      logged_task_stacks = true;
      mem_diag::LogTaskStacks(kTag);
    }

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

    // M15: drain at most one API command per loop tick, on this same app
    // task -- mirrors the encoder-poll block immediately above exactly
    // (DialAction -> GroheClient call -> HandleCommandSent()), just
    // triggered by the httpd task's enqueue (see App::RequestDispense()/
    // RequestStop() below) instead of a physical button press. Zero
    // timeout: never blocks this loop waiting for a command that isn't
    // there yet.
    ApiCommand api_command;
    if (xQueueReceive(api_command_queue_, &api_command, 0) == pdTRUE) {
      if (api_command.kind == ApiCommand::Kind::kStatusQuery) {
        // M15.1: a plain, same-task read -- DialController::State() is
        // only safe to call from this task (see its own header comment)
        // -- posted back through api_status_queue_ rather than
        // api_result_queue_ (see that member's own comment on app.hpp
        // for why they're separate). No DialAction, no GroheClient call,
        // no HandleCommandSent(): a status query never changes state, so
        // it deliberately skips the switch below entirely -- but must
        // still fall through to this loop's own rendering/backlight/
        // vTaskDelay tail below, not `continue` past it (that would
        // starve the scheduler of this task's own yield point).
        const dial_state::DialState snapshot = dial_controller_.State();
        xQueueOverwrite(api_status_queue_, &snapshot);
      } else {
        DialAction action = DialAction::kNone;
        switch (api_command.kind) {
          case ApiCommand::Kind::kDispense:
            action = dial_controller_.RequestDispenseAction(api_command.amount_ml,
                                                             api_command.water_type);
            break;
          case ApiCommand::Kind::kStop:
            action = dial_controller_.RequestStopAction();
            break;
          case ApiCommand::Kind::kStatusQuery:
            break;  // Handled above; unreachable here.
        }
        dial_api::RequestResult result = dial_api::RequestResult::kRejectedNotAvailable;
        switch (action) {
          case app::DialAction::kRequestDispense: {
            state_changed = true;
            const auto& state = dial_controller_.State();
            const bool accepted = grohe_client_.RequestDispense(
                state.amount_ml, app::ToGroheWaterType(state.water_type));
            dial_controller_.HandleCommandSent(accepted);
            result = accepted ? dial_api::RequestResult::kAccepted
                               : dial_api::RequestResult::kRejectedNotReady;
            break;
          }
          case app::DialAction::kRequestStop: {
            state_changed = true;
            const bool accepted = grohe_client_.RequestStop();
            dial_controller_.HandleCommandSent(accepted);
            result = accepted ? dial_api::RequestResult::kAccepted
                               : dial_api::RequestResult::kRejectedNotReady;
            break;
          }
          case app::DialAction::kNone:
            result = dial_api::RequestResult::kRejectedNotAvailable;
            break;
        }
        // Length-1 queue, always overwrite rather than plain send: the
        // caller (RequestDispense()/RequestStop(), blocked on
        // xQueueReceive() with a bounded timeout) is the only consumer
        // and is always waiting by the time this runs, but
        // xQueueOverwrite() guarantees this post can never itself
        // block/fail even in an edge case where the caller already gave
        // up.
        xQueueOverwrite(api_result_queue_, &result);
      }
    }

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

    vTaskDelay(kPollPeriod);
  }
}

// M15: dial_api::DialApiHandler overrides -- see that interface's own
// comment for the contract, and app.hpp for why this exists as a
// separate small component rather than App/ProvisioningServer depending
// on each other directly.

dial_state::DialState App::Status() const {
  // M15.1: cross-task hand-off, mirroring RequestDispense()/RequestStop()
  // below exactly (same api_command_queue_, same kApiQueueTimeout bound)
  // -- see api_status_queue_'s own comment on app.hpp for why the result
  // travels back on its own queue rather than api_result_queue_.
  //
  // Replaces a previous direct `return dial_controller_.State();` here,
  // which called DialController::State() directly from the httpd task.
  // That looked safe at a glance (dial_state::DialState is a small POD,
  // single-core chip, no torn *individual* field), but the actual risk
  // was never a torn field -- it was a torn *combination* of fields: the
  // app task updates several related fields of the same DialState across
  // separate statements for one logical transition (e.g.
  // HandleCommandOutcome()'s dispense_status / active_dispense_amount_ml
  // / delivered_ml trio, or Tick()'s own multi-field resets), and
  // FreeRTOS can preempt the app task between any two of those
  // statements. A reader on another task copying the whole struct in
  // that window could observe a combination -- dispense_status already
  // DISPENSING but active_dispense_amount_ml still the *previous*
  // dispense's amount, say -- that never existed as a real state, not
  // just a stale-but-coherent one. Routing the read through the app task
  // too (the same place every write already happens) closes that window
  // instead of arguing it away.
  const ApiCommand command{.kind = ApiCommand::Kind::kStatusQuery};
  if (xQueueSend(api_command_queue_, &command, kApiQueueTimeout) == pdTRUE) {
    dial_state::DialState snapshot{};
    if (xQueueReceive(api_status_queue_, &snapshot, kApiQueueTimeout) == pdTRUE) {
      // Cached for the timeout fallback below -- see last_known_status_'s
      // own comment on app.hpp. Only ever written here, and only this
      // one httpd-task entry point ever calls Status() at all
      // (esp_http_server's single-worker-task model -- see
      // api_command_queue_'s own comment -- means this is never itself
      // written from two tasks, or even two overlapping calls, at once).
      last_known_status_ = snapshot;
      return snapshot;
    }
  }
  // The bounded round-trip above failed -- the app task's own loop has
  // stalled far beyond its normal 20ms cadence, the same anomaly
  // RequestDispense()/RequestStop() already treat as kTimeout (logged by
  // provisioning_server.cpp's SendRequestResult()). GET /api/status has
  // no error channel of its own in the documented API contract (see
  // docs/m15_ha_integration.md) to surface this the same way without
  // changing that contract, so this falls back to the last snapshot that
  // *did* complete a full, consistent round-trip -- possibly stale by
  // now, but still a real state the dial actually was in, unlike a
  // fabricated default-constructed DialState (which would misrepresent
  // connection_status/dispense_status as "never detected"), and unlike
  // falling back to a direct dial_controller_.State() read here (which
  // would reintroduce the exact torn-read risk this method exists to
  // close, right in its own error path). See DialController::State()'s
  // own comment: nothing on the httpd task may call it directly, no
  // exceptions, including this one.
  ESP_LOGE(kTag, "Status query timed out waiting for the app task -- "
           "this should not happen at the normal 20ms loop cadence");
  return last_known_status_;
}

settings::DialSettings App::Config() const {
  // Unlike Status() above, this one isn't actually cross-task in
  // practice: dial_controller_.ApplySettings(dial_settings_.Values())
  // (Run()'s own boot sequence) is the *only* app-task read of
  // dial_settings_ that ever happens, and it completes before
  // provisioning_server_.Init() can possibly start the HTTP server that
  // reaches this method -- every call to Config()/SetConfig() after that
  // point comes exclusively from the httpd task. No synchronization
  // needed for a resource only one task ever touches after boot.
  return dial_settings_.Values();
}

bool App::SetConfig(const settings::DialSettings& new_values) {
  // See Config()'s own comment -- dial_settings_ is httpd-task-exclusive
  // after boot, so this is a plain call, no queue/hand-off needed.
  // Deliberately does not also call dial_controller_.ApplySettings()
  // again -- see dial_api::DialApiHandler::SetConfig()'s own comment for
  // why forcing the dial's live amount/water-type to the new defaults
  // mid-interaction would be wrong.
  return dial_settings_.Set(new_values);
}

dial_api::RequestResult App::RequestDispense(int amount_ml,
                                             dial_state::WaterType water_type) {
  const ApiCommand command{.kind = ApiCommand::Kind::kDispense,
                           .amount_ml = amount_ml,
                           .water_type = water_type};
  // Cross-task hand-off -- see api_command_queue_/api_result_queue_'s own
  // comment on app.hpp, and the drain loop in Run() above for the other
  // half. Bounded wait, not indefinite: a send or receive that actually
  // times out here means the app task's own loop has stalled far beyond
  // its normal 20ms cadence -- a real fault worth surfacing as
  // dial_api::RequestResult::kTimeout, not something to retry silently.
  if (xQueueSend(api_command_queue_, &command, kApiQueueTimeout) != pdTRUE) {
    return dial_api::RequestResult::kTimeout;
  }
  dial_api::RequestResult result = dial_api::RequestResult::kTimeout;
  xQueueReceive(api_result_queue_, &result, kApiQueueTimeout);
  return result;
}

dial_api::RequestResult App::RequestStop() {
  const ApiCommand command{.kind = ApiCommand::Kind::kStop};
  if (xQueueSend(api_command_queue_, &command, kApiQueueTimeout) != pdTRUE) {
    return dial_api::RequestResult::kTimeout;
  }
  dial_api::RequestResult result = dial_api::RequestResult::kTimeout;
  xQueueReceive(api_result_queue_, &result, kApiQueueTimeout);
  return result;
}

}  // namespace app
