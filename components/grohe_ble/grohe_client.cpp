#include "grohe_ble/grohe_client.hpp"

#include <cstring>
#include <ctime>

#include "esp_log.h"

namespace grohe_ble {
namespace {
constexpr char kTag[] = "grohe_client";
}  // namespace

esp_err_t GroheClient::Init() { return ble_manager_.Init(); }

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
        // A fresh connection (if a future milestone adds reconnect) starts
        // this sequence over -- see the members' own comment in
        // grohe_client.hpp.
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
          ? BuildStopPayload(credentials_provider_.Get(),
                             static_cast<uint32_t>(time(nullptr)), payload,
                             sizeof(payload))
          : BuildDispensePayload(credentials_provider_.Get(), amount_ml,
                                 taste, static_cast<uint32_t>(time(nullptr)),
                                 payload, sizeof(payload));
  if (!built) {
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
