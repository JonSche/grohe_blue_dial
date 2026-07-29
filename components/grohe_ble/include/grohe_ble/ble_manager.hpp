#pragma once

#include <functional>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace grohe_ble {

// Full BLE connection lifecycle. Only Idle -> Initializing -> Scanning is
// actually reachable in M3.1 (there is no scan/connect/discover
// implementation yet); the remaining states exist so the framework doesn't
// need to change shape when a future milestone fills them in.
enum class BleState {
  kIdle,
  kInitializing,
  kScanning,
  kConnecting,
  kDiscovering,
  kReady,
  kDisconnected,
  kBackoff,
};

[[nodiscard]] constexpr const char* ToString(BleState state) {
  switch (state) {
    case BleState::kIdle:
      return "Idle";
    case BleState::kInitializing:
      return "Initializing";
    case BleState::kScanning:
      return "Scanning";
    case BleState::kConnecting:
      return "Connecting";
    case BleState::kDiscovering:
      return "Discovering";
    case BleState::kReady:
      return "Ready";
    case BleState::kDisconnected:
      return "Disconnected";
    case BleState::kBackoff:
      return "Backoff";
  }
  return "";
}

// Only the events M3.1 can actually produce. Scan/connect/discovery events
// (DeviceFound, Connected, ServiceDiscovered, ...) belong to a future
// milestone, once there is code that can actually raise them.
enum class BleEventType {
  kHostSynced,
  kHostReset,
};

[[nodiscard]] constexpr const char* ToString(BleEventType type) {
  switch (type) {
    case BleEventType::kHostSynced:
      return "HostSynced";
    case BleEventType::kHostReset:
      return "HostReset";
  }
  return "";
}

struct BleEvent {
  BleEventType type;
  int reason = 0;  // Only meaningful for kHostReset.
};

// Owns the NimBLE host stack lifecycle and the BLE state machine. NimBLE's
// GAP/GATT callbacks fire on NimBLE's own host task, never on the caller's
// task -- BleManager hands them off through a bounded queue so nothing
// outside PollEvents() ever runs on that task's stack (see
// docs/ARCHITECTURE.md's threading section). Callbacks do the minimum
// possible work: build a BleEvent and enqueue it.
//
// M3.1 scope: initialize the host and expose the state-machine skeleton
// only. No scanning, connecting, or discovery -- that's grohe_ble's next
// milestone.
class BleManager {
 public:
  BleManager() = default;
  ~BleManager();

  BleManager(const BleManager&) = delete;
  BleManager& operator=(const BleManager&) = delete;

  // Initializes NVS (required by NimBLE), starts the NimBLE host stack and
  // its dedicated FreeRTOS task. Safe to call exactly once. Returns an
  // error rather than aborting -- BLE failing to start must not prevent
  // the rest of the firmware (display, encoder, UI) from running.
  esp_err_t Init();

  [[nodiscard]] BleState State() const;

  // Drains events enqueued since the last call and invokes on_event for
  // each, on the calling task. Call periodically from the app loop; never
  // call from a NimBLE callback.
  void PollEvents(const std::function<void(const BleEvent&)>& on_event);

 private:
  static void HostTask(void* param);
  static void OnHostSync();
  static void OnHostReset(int reason);

  void SetState(BleState state);
  void Enqueue(const BleEvent& event);

  QueueHandle_t event_queue_ = nullptr;
  BleState state_ = BleState::kIdle;

  // NimBLE's ble_hs_cfg callbacks are plain C function pointers with no
  // user-data parameter, so the static callbacks above reach instance state
  // through this back-pointer -- the same pattern already used by
  // Gc9a01Display for the same reason (see gc9a01_display.hpp). Exactly one
  // BleManager instance ever exists (owned by GroheClient, owned by App),
  // matching Gc9a01Display's own lifetime.
  static BleManager* instance_;
};

}  // namespace grohe_ble
