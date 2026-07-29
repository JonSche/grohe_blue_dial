#pragma once

#include <functional>

#include "esp_err.h"
#include "grohe_ble/ble_manager.hpp"

namespace grohe_ble {

// The one class app:: is allowed to talk to for BLE -- App never touches
// BleManager directly. This is where anything specific to the Grohe Blue
// (its advertised identity, its GATT protocol) will live once a future
// milestone adds scanning, connecting, and discovery.
//
// M3.1 scope: a thin pass-through over BleManager. There is no Grohe-
// specific identity or filtering yet, so there is nothing here to add
// beyond forwarding Init()/Poll() -- adding a parallel GroheEvent type that
// would just mirror BleEvent right now would be exactly the kind of
// speculative interface this milestone is explicit about avoiding.
class GroheClient {
 public:
  GroheClient() = default;

  esp_err_t Init();

  void Poll(const std::function<void(const BleEvent&)>& on_event);

 private:
  BleManager ble_manager_;
};

}  // namespace grohe_ble
