#include "grohe_ble/grohe_client.hpp"

namespace grohe_ble {

esp_err_t GroheClient::Init() { return ble_manager_.Init(); }

void GroheClient::Poll(const std::function<void(const BleEvent&)>& on_event) {
  ble_manager_.PollEvents(on_event);
  ble_manager_.PollCharacteristicEvents(
      [this](const BleCharacteristicEvent& event) {
        protocol_.HandleCharacteristicEvent(event);
      });
}

}  // namespace grohe_ble
