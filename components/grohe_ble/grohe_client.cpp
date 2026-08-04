#include "grohe_ble/grohe_client.hpp"

#include <cstring>

#include "esp_log.h"

namespace grohe_ble {
namespace {
constexpr char kTag[] = "grohe_client";
}  // namespace

esp_err_t GroheClient::Init() {
  // Non-fatal, same reasoning as BleManager's own failures below: a Wi-Fi/
  // SNTP problem must not take down BLE, the display, or the encoder. Time
  // and BLE are independent, so order between the two Init() calls doesn't
  // matter functionally -- starting the (non-blocking, event-driven) time
  // sync first just gives it the most possible head start before a command
  // is ever likely to be sent.
  const esp_err_t time_err = time_provider_.Init();
  if (time_err != ESP_OK) {
    ESP_LOGE(kTag, "SntpTimeProvider::Init() failed: %s",
             esp_err_to_name(time_err));
  }
  return ble_manager_.Init();
}

void GroheClient::Poll(const std::function<void(const BleEvent&)>& on_event) {
  ble_manager_.PollEvents([this, &on_event](const BleEvent& event) {
    on_event(event);
    switch (event.type) {
      case BleEventType::kReadyForProtocol:
        ready_for_protocol_ = true;
        break;
      case BleEventType::kSubscribed:
        subscribed_ = true;
        break;
      case BleEventType::kConnectionFailed:
        // A fresh connection -- BleManager retries automatically as of
        // M11.1 -- starts this sequence over -- see the members' own
        // comment in grohe_client.hpp.
        ready_for_protocol_ = false;
        subscribed_ = false;
        pending_command_ = PendingCommand::kNone;
        break;
      default:
        break;
    }
  });
  ble_manager_.PollCharacteristicEvents(
      [this](const BleCharacteristicEvent& event) {
        protocol_.HandleCharacteristicEvent(event);
      });
}

bool GroheClient::RequestDispense(int amount_ml, WaterType taste) {
  return SendCommand(PendingCommand::kDispense, amount_ml, taste);
}

bool GroheClient::RequestStop() {
  return SendCommand(PendingCommand::kStop, 0, WaterType::kUnknown);
}

bool GroheClient::SendCommand(PendingCommand kind, int amount_ml,
                              WaterType taste) {
  if (pending_command_ != PendingCommand::kNone) {
    ESP_LOGW(kTag, "command already in flight, ignoring new request");
    return false;
  }
  if (!ready_for_protocol_ || !subscribed_) {
    ESP_LOGW(kTag, "not ready for protocol commands yet");
    return false;
  }
  const uint16_t write_handle = protocol_.write_char_handle();
  if (write_handle == 0) {
    // Structurally not expected on this hardware (M5 confirmed the write
    // characteristic is always present), but nothing here should assume
    // that rather than check it.
    ESP_LOGE(kTag, "write characteristic never found");
    return false;
  }

  char payload[kMaxStopPayloadSize];
  const bool built =
      (kind == PendingCommand::kStop)
          ? BuildStopPayload(credentials_provider_.Get(), time_provider_,
                             payload, sizeof(payload))
          : BuildDispensePayload(credentials_provider_.Get(), amount_ml,
                                 taste, time_provider_, payload,
                                 sizeof(payload));
  if (!built) {
    // Covers both an HMAC/buffer failure and "no valid time available yet"
    // (M9) -- BuildDispensePayload()/BuildStopPayload() don't distinguish
    // the reason at this level, and this class already rejects a command
    // the same way regardless of which it was.
    ESP_LOGE(kTag, "failed to build command payload");
    return false;
  }

  const size_t payload_len = std::strlen(payload);
  const esp_err_t err = ble_manager_.WriteCharacteristic(
      write_handle, reinterpret_cast<const uint8_t*>(payload), payload_len);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "WriteCharacteristic failed: %s", esp_err_to_name(err));
    return false;
  }

  ESP_LOGI(kTag, "%s command queued (%u bytes)",
          kind == PendingCommand::kStop ? "stop" : "dispense",
          static_cast<unsigned>(payload_len));
  pending_command_ = kind;
  pending_command_baseline_sequence_ = protocol_.State().sequence;
  return true;
}

CommandOutcome GroheClient::TakeCommandOutcome() {
  CommandOutcome outcome;
  if (pending_command_ == PendingCommand::kNone) {
    return outcome;  // available == false
  }
  const ApplianceState& state = protocol_.State();
  if (!state.received || state.sequence == pending_command_baseline_sequence_) {
    return outcome;  // No new response since this command was sent.
  }

  outcome.available = true;
  outcome.was_dispense = (pending_command_ == PendingCommand::kDispense);
  outcome.state = state;
  pending_command_ = PendingCommand::kNone;
  return outcome;
}

}  // namespace grohe_ble
