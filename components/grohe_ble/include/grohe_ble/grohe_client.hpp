#pragma once

#include <functional>

#include "esp_err.h"
#include "grohe_ble/ble_manager.hpp"
#include "grohe_ble/grohe_protocol.hpp"

namespace grohe_ble {

// The one class app:: is allowed to talk to for BLE -- App never touches
// BleManager directly.
//
// M6 scope: Poll()'s public signature and behavior are exactly what M3.1
// established (drain the lifecycle queue, forward each event to the
// caller's callback) -- App::Run()'s call site does not change at all.
// Internally, Poll() now also drains BleManager's separate characteristic
// queue and feeds it to an owned GroheProtocol, which is where the actual
// Grohe protocol interpretation happens (see grohe_protocol.hpp). This is
// the only class that knows both BleManager and GroheProtocol exist.
class GroheClient {
 public:
  GroheClient() = default;

  esp_err_t Init();

  void Poll(const std::function<void(const BleEvent&)>& on_event);

 private:
  BleManager ble_manager_;
  GroheProtocol protocol_;
};

}  // namespace grohe_ble
