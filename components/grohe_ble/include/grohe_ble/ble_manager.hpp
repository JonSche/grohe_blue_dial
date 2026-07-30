#pragma once

#include <functional>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nimble/ble.h"

// NimBLE GAP types, needed only as callback parameters -- forward-declared
// so consumers of this header (GroheClient, and through it app::App) don't
// transitively pull in host/ble_gap.h.
struct ble_gap_event;
struct ble_gap_disc_desc;

namespace grohe_ble {

// Full BLE connection lifecycle. Only Idle -> Initializing -> Scanning ->
// DeviceFound is actually reachable as of M4 (scanning is implemented;
// connecting and GATT discovery are not); the remaining states exist so the
// framework doesn't need to change shape when a future milestone fills them
// in.
enum class BleState {
  kIdle,
  kInitializing,
  kScanning,
  kDeviceFound,
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
    case BleState::kDeviceFound:
      return "DeviceFound";
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

// Only the events this firmware can actually produce. Connect/discovery
// events (Connected, ServiceDiscovered, ...) belong to a future milestone,
// once there is code that can actually raise them.
enum class BleEventType {
  kHostSynced,
  kHostReset,
  kDeviceFound,
};

[[nodiscard]] constexpr const char* ToString(BleEventType type) {
  switch (type) {
    case BleEventType::kHostSynced:
      return "HostSynced";
    case BleEventType::kHostReset:
      return "HostReset";
    case BleEventType::kDeviceFound:
      return "DeviceFound";
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
// Scope as of M4: initialize the host, then actively scan until the Grohe
// Blue appliance is identified by its advertised service UUID (see
// ble_constants.hpp), at which point scanning stops and the appliance's
// address is recorded. No connecting, GATT discovery, or authentication --
// that's grohe_ble's next milestone.
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

  // GAP callback for the discovery procedure. Unlike ble_hs_cfg's callbacks
  // this one does take a user-data pointer, so it receives `this` through
  // ble_gap_disc()'s cb_arg rather than going through instance_.
  static int OnGapEvent(struct ble_gap_event* event, void* arg);

  // Starts the active scan. Runs on the NimBLE host task (from OnHostSync).
  esp_err_t StartScan();

  // Handles one advertising report: parse, log, and -- if this is the Grohe
  // Blue -- stop scanning and record it. Runs on the NimBLE host task.
  void HandleDiscReport(const struct ble_gap_disc_desc& disc);

  void SetState(BleState state);
  void Enqueue(const BleEvent& event);

  QueueHandle_t event_queue_ = nullptr;
  BleState state_ = BleState::kIdle;

  // The appliance's address, recorded once it is discovered. Only meaningful
  // from kDeviceFound onwards -- a host reset returns the state machine to
  // kIdle without clearing this, and the next successful discovery
  // overwrites it, so read the state before trusting it.
  //
  // Deliberately has no accessor yet: the code that will consume it
  // (ble_gap_connect()) belongs to this same class in a later milestone, so
  // exposing it now would be a speculative interface. It is logged on
  // discovery, which is what makes the stored value observable today.
  ble_addr_t device_addr_ = {};

  // NimBLE's ble_hs_cfg callbacks are plain C function pointers with no
  // user-data parameter, so the static callbacks above reach instance state
  // through this back-pointer -- the same pattern already used by
  // Gc9a01Display for the same reason (see gc9a01_display.hpp). Exactly one
  // BleManager instance ever exists (owned by GroheClient, owned by App),
  // matching Gc9a01Display's own lifetime.
  static BleManager* instance_;
};

}  // namespace grohe_ble
