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

// How long the Finished checkmark stays on screen before returning to
// Ready (frozen UI spec, "Stop and Finished": "~400 ms").
constexpr int64_t kFinishedHoldUs = 400'000;

// M11.1: how long the dial shows "Connection lost" before switching to
// "Connecting..." for the remainder of BleManager's own (much longer,
// backing-off) retry loop -- "a short visible indication (~1 second)" per
// the milestone's own spec.
constexpr int64_t kConnectionLostHoldUs = 1'000'000;

// How long to keep TimeStatus at kSyncing before falling back to
// kUnavailable once the BLE connection is ready but a valid epoch still
// hasn't arrived. TimeProvider itself has no concept of "given up" -- SNTP
// either resolves or the Wi-Fi/SNTP window (M9's architecture) just hasn't
// completed yet -- so this is a UI-only judgement call, not a firmware
// timeout. 15 s is a generous multiple of the few seconds that window is
// expected to take, chosen so a slow but still-succeeding sync is never
// mislabeled as broken. (As of M11.1 both states render identically --
// "Synchronising..." -- but the distinction stays meaningful internally,
// e.g. for connected_since_us_-relative reasoning elsewhere.)
constexpr int64_t kTimeSyncTimeoutUs = 15'000'000;

// Floors to the 10-ml grid the frozen UI spec's count-up cadence expects
// ("0 -> 10 -> 20 -> ... -> 500 ml") -- deliberately rounds down, not to
// the nearest 10, so the displayed value is always strictly less than the
// dialled amount for as long as any time genuinely remains (remaining_ml >
// 0 implies floor(remaining_ml) < target). Rounding to the *nearest* 10
// could round the last fraction of a second's value up to the full target
// while dispense_status was still kDispensing -- i.e. the numeral would
// briefly claim "done" before the state machine agreed, undermining the
// one property (the numeral is always an honest, real value) this whole
// design depends on. The checkmark, not this number, is what claims 100%.
constexpr int RoundDownToNearest10Ml(int64_t value_ml) {
  return static_cast<int>(value_ml / 10 * 10);
}
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
      switch (state_.dispense_status) {
        case dial_state::DispenseStatus::kIdle:
          ESP_LOGI(kTag, "Dispense requested: %d ml", state_.amount_ml);
          pending_dispense_amount_ml_ = state_.amount_ml;
          return DialAction::kRequestDispense;
        case dial_state::DispenseStatus::kDispensing:
          ESP_LOGI(kTag, "Stop requested");
          // Optimistic: show "STOPPING..." immediately rather than only
          // once the acknowledgement arrives (frozen UI spec, "Stop and
          // Finished"). HandleCommandOutcome() reverts this to kDispensing
          // if the stop is actually rejected.
          state_.dispense_status = dial_state::DispenseStatus::kStopping;
          return DialAction::kRequestStop;
        case dial_state::DispenseStatus::kStopping:
        case dial_state::DispenseStatus::kFinished:
          break;  // Already committed to stopping, or about to auto-return.
      }
      break;
    case EncoderEvent::kLongPress:
      // M10: three-way cycle, Still -> Medium -> Sparkling -> Still. An
      // explicit switch (not modular arithmetic on the enum's underlying
      // value) so a future fourth water type fails to compile here
      // (-Wswitch, no default: label) rather than silently cycling wrong.
      if (state_.dispense_status == dial_state::DispenseStatus::kIdle) {
        switch (state_.water_type) {
          case dial_state::WaterType::kStill:
            state_.water_type = dial_state::WaterType::kMedium;
            break;
          case dial_state::WaterType::kMedium:
            state_.water_type = dial_state::WaterType::kSparkling;
            break;
          case dial_state::WaterType::kSparkling:
            state_.water_type = dial_state::WaterType::kStill;
            break;
        }
      }
      break;
  }
  return DialAction::kNone;
}

void DialController::HandleCommandSent(bool accepted) {
  command_pending_ = accepted;
  if (!accepted &&
      state_.dispense_status == dial_state::DispenseStatus::kStopping) {
    // HandleEvent()'s optimistic "STOPPING..." transition assumed the stop
    // request would actually be sent; GroheClient::RequestStop() itself
    // returned false, so no acknowledgement is coming to revert it via
    // HandleCommandOutcome(). Revert here instead -- otherwise the UI is
    // stuck on "STOPPING..." forever, since a dispense request never
    // touches dispense_status this early, this branch can only ever be
    // reached for the stop it actually applies to.
    state_.dispense_status = dial_state::DispenseStatus::kDispensing;
  }
}

bool DialController::HandleCommandOutcome(
    const grohe_ble::CommandOutcome& outcome) {
  if (!outcome.available) {
    return false;
  }
  command_pending_ = false;

  if (!outcome.state.is_success) {
    ESP_LOGW(kTag, "%s command rejected: code=%ld",
             outcome.was_dispense ? "dispense" : "stop",
             outcome.state.response_code);
    if (!outcome.was_dispense &&
        state_.dispense_status == dial_state::DispenseStatus::kStopping) {
      // The stop didn't actually take effect -- gracefully handled, not
      // invented: revert the optimistic "STOPPING..." back to the normal
      // dispensing UI, relying on dispense_session_ (never touched here,
      // still running) to finish naturally.
      state_.dispense_status = dial_state::DispenseStatus::kDispensing;
      return true;
    }
    // A rejected dispense request leaves dispense_status exactly as it
    // was (still Idle) -- nothing to revert.
    return false;
  }

  if (outcome.was_dispense) {
    state_.dispense_status = dial_state::DispenseStatus::kDispensing;
    state_.active_dispense_amount_ml = pending_dispense_amount_ml_;
    state_.delivered_ml = 0;
    dispense_session_.Start(pending_dispense_amount_ml_, esp_timer_get_time());
  } else if (state_.dispense_status == dial_state::DispenseStatus::kStopping) {
    state_.dispense_status = dial_state::DispenseStatus::kIdle;
    state_.delivered_ml = 0;
    dispense_session_.Stop();
  } else {
    // A stop acknowledgement that arrives after the UI has already moved
    // on -- via Tick()'s own forward-progress fallback (see its kStopping
    // branch) once dispense_session_ finished naturally before this ack
    // arrived, or via a connection-loss force-to-Idle -- is stale. Applying
    // it now would stomp whatever more-current state the dial is already
    // in, e.g. cutting the Finished checkmark's hold short.
    return false;
  }
  return true;
}

bool DialController::Tick() {
  const int64_t now_us = esp_timer_get_time();

  // M11.1: connection_lost_until_us_'s own hold has elapsed -- BleManager
  // is still retrying (it retries indefinitely on its own backoff
  // schedule; this dial-side timer is unrelated to and much shorter than
  // that one), so the dial now reads "Connecting..." for as long as that
  // continues. A fresh failure re-enters kConnectionLost via
  // HandleConnectionLost(), independently of dispense_status below.
  if (state_.connection_status == dial_state::ConnectionStatus::kConnectionLost &&
      now_us >= connection_lost_until_us_) {
    state_.connection_status = dial_state::ConnectionStatus::kConnecting;
    return true;
  }

  if (state_.dispense_status == dial_state::DispenseStatus::kDispensing) {
    if (dispense_session_.Finished(now_us)) {
      dispense_session_.Stop();
      state_.dispense_status = dial_state::DispenseStatus::kFinished;
      finished_until_us_ = now_us + kFinishedHoldUs;
      return true;
    }
    // Count-up (frozen UI spec, "Amount -- count-up"): delivered volume is
    // the complement of DispenseSession's own remaining fraction, rounded
    // to the 10 ml cadence so this only actually changes -- and only then
    // triggers App::Run()'s re-render -- a few times a second, never every
    // 20 ms poll.
    const int64_t duration_us = dispense_session_.DurationUs();
    const int64_t remaining_us = dispense_session_.Remaining(now_us);
    const int64_t delivered_raw_ml =
        duration_us > 0
            ? static_cast<int64_t>(state_.active_dispense_amount_ml) *
                  (duration_us - remaining_us) / duration_us
            : 0;
    const int rounded_ml = RoundDownToNearest10Ml(delivered_raw_ml);
    if (rounded_ml == state_.delivered_ml) {
      return false;
    }
    state_.delivered_ml = rounded_ml;
    return true;
  }

  if (state_.dispense_status == dial_state::DispenseStatus::kStopping) {
    // Forward-progress fallback: entering kStopping never stops
    // dispense_session_ (only a successful outcome does, above) -- so if
    // the stop acknowledgement is ever lost entirely, the physical timer
    // is still the backstop it always was pre-kStopping. Without this, a
    // lost ack left the UI on "STOPPING..." forever, since dispense_status
    // no longer being kDispensing meant this method never looked at
    // dispense_session_ again. A late-arriving ack after this fires is
    // simply ignored (see HandleCommandOutcome()'s own guard).
    if (dispense_session_.Finished(now_us)) {
      dispense_session_.Stop();
      state_.dispense_status = dial_state::DispenseStatus::kFinished;
      finished_until_us_ = now_us + kFinishedHoldUs;
      return true;
    }
    return false;
  }

  if (state_.dispense_status == dial_state::DispenseStatus::kFinished) {
    if (now_us < finished_until_us_) {
      return false;
    }
    state_.dispense_status = dial_state::DispenseStatus::kIdle;
    state_.delivered_ml = 0;
    return true;
  }

  return false;
}

bool DialController::HandleConnectionLost() {
  command_pending_ = false;
  dispense_session_.Stop();
  ble_ready_for_protocol_ = false;
  ble_subscribed_ = false;

  bool changed = false;
  if (state_.dispense_status != dial_state::DispenseStatus::kIdle) {
    state_.dispense_status = dial_state::DispenseStatus::kIdle;
    state_.delivered_ml = 0;
    changed = true;
  }
  if (state_.connection_status != dial_state::ConnectionStatus::kConnectionLost) {
    state_.connection_status = dial_state::ConnectionStatus::kConnectionLost;
    changed = true;
  }
  // M11.1: (re)start the hold regardless of whether connection_status just
  // changed -- this call means BleManager just failed a connection or
  // reconnection attempt, even if that's not the first one since the link
  // first dropped (e.g. a retry attempt itself failed after Tick() had
  // already moved the dial on to "Connecting..."). Each genuine failure
  // deserves its own brief, honest "Connection lost" moment.
  connection_lost_until_us_ = esp_timer_get_time() + kConnectionLostHoldUs;
  return changed;
}

bool DialController::HandleReadyForProtocol() {
  if (ble_ready_for_protocol_) {
    return false;
  }
  ble_ready_for_protocol_ = true;
  return UpdateConnectionStatus();
}

bool DialController::HandleSubscribed() {
  if (ble_subscribed_) {
    return false;
  }
  ble_subscribed_ = true;
  return UpdateConnectionStatus();
}

bool DialController::UpdateConnectionStatus() {
  const dial_state::ConnectionStatus next =
      (ble_ready_for_protocol_ && ble_subscribed_)
          ? dial_state::ConnectionStatus::kReady
          : dial_state::ConnectionStatus::kConnecting;
  if (next == state_.connection_status) {
    return false;
  }
  if (next == dial_state::ConnectionStatus::kReady) {
    connected_since_us_ = esp_timer_get_time();
  }
  state_.connection_status = next;
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

  // M11.1: this used to also drive a UI label ("APPL OK" / "APPL CODE n");
  // that's gone now (raw protocol response codes aren't appropriate for a
  // production UI), so the log line below is the only place this is still
  // observable -- diagnostic, not a report of a user-visible error.
  const char* name = dial_state::ResponseCodeName(state_.appliance_response_code);
  if (state_.appliance_response_success) {
    ESP_LOGI(kTag, "Appliance response: OK");
  } else if (name != nullptr) {
    ESP_LOGW(kTag, "Appliance response: %s", name);
  } else {
    ESP_LOGW(kTag, "Appliance response: CODE %d", state_.appliance_response_code);
  }
  return true;
}

bool DialController::HandleTimeStatus(bool available) {
  // Recomputed every call (App::Run() calls this every ~20 ms poll, same as
  // Tick()) rather than only reacting to `available` flipping, since
  // kSyncing -> kUnavailable can happen purely from elapsed time, with
  // `available` staying false throughout -- see TimeStatus's own comment
  // on dial_state.hpp for why this threshold exists at all.
  dial_state::TimeStatus next;
  if (available) {
    next = dial_state::TimeStatus::kAvailable;
  } else if (state_.connection_status != dial_state::ConnectionStatus::kReady) {
    // The Wi-Fi/SNTP window (M9) only ever runs once the BLE link itself
    // is up, so there's nothing to measure a timeout against yet. This
    // renders identically to a genuine timeout (both are "not available"),
    // which is exactly the right visual: the tiny time glyph has no
    // meaningful "syncing" motion to show before a sync attempt has even
    // had a chance to start.
    next = dial_state::TimeStatus::kUnavailable;
  } else {
    const int64_t elapsed_us = esp_timer_get_time() - connected_since_us_;
    next = elapsed_us < kTimeSyncTimeoutUs ? dial_state::TimeStatus::kSyncing
                                           : dial_state::TimeStatus::kUnavailable;
  }
  if (next == state_.time_status) {
    return false;
  }
  state_.time_status = next;
  return true;
}

grohe_ble::WaterType ToGroheWaterType(dial_state::WaterType type) {
  switch (type) {
    case dial_state::WaterType::kStill:
      return grohe_ble::WaterType::kStill;
    case dial_state::WaterType::kMedium:
      return grohe_ble::WaterType::kMedium;
    case dial_state::WaterType::kSparkling:
      return grohe_ble::WaterType::kSparkling;
  }
  return grohe_ble::WaterType::kUnknown;
}

}  // namespace app
