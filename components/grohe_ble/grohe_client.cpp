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
        stop_probe_sent_ = false;
        break;
      default:
        break;
    }
  });
  ble_manager_.PollCharacteristicEvents(
      [this](const BleCharacteristicEvent& event) {
        protocol_.HandleCharacteristicEvent(event);
      });

  MaybeSendStopProbe();
}

void GroheClient::MaybeSendStopProbe() {
  if (stop_probe_sent_ || !ready_for_protocol_ || !subscribed_) {
    return;
  }
  const uint16_t write_handle = protocol_.write_char_handle();
  if (write_handle == 0) {
    // Structurally not expected on this hardware (M5 confirmed the write
    // characteristic is always present), but nothing here should assume
    // that rather than check it.
    ESP_LOGW(kTag, "write characteristic never found; not sending stop() probe");
    stop_probe_sent_ = true;  // Never retry -- see the member's own comment.
    return;
  }

  char payload[kMaxStopPayloadSize];
  if (!BuildStopPayload(credentials_provider_.Get(),
                       static_cast<uint32_t>(time(nullptr)), payload,
                       sizeof(payload))) {
    ESP_LOGE(kTag, "failed to build stop() probe payload");
    stop_probe_sent_ = true;  // Never retry -- see the member's own comment.
    return;
  }

  const size_t payload_len = std::strlen(payload);
  const esp_err_t err = ble_manager_.WriteCharacteristic(
      write_handle, reinterpret_cast<const uint8_t*>(payload), payload_len);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "WriteCharacteristic failed: %s", esp_err_to_name(err));
  } else {
    ESP_LOGI(kTag, "stop() probe queued (%u bytes)",
             static_cast<unsigned>(payload_len));
  }
  // Sent exactly once per connection regardless of the transport result --
  // this milestone's stop() probe is infrastructure, not a user command,
  // and does not retry (matches M6's carried-forward "do not retry
  // automatically" policy). A transport-level failure here already leads
  // to a clean disconnect via BleManager's own FailConnection() path.
  stop_probe_sent_ = true;
}

}  // namespace grohe_ble
