#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nimble/ble.h"

// NimBLE GAP/GATT types, needed only as callback parameters -- forward-
// declared so consumers of this header (GroheClient, and through it
// app::App) don't transitively pull in host/ble_gap.h or host/ble_gatt.h.
struct ble_gap_event;
struct ble_gap_disc_desc;
struct ble_gatt_error;
struct ble_gatt_svc;
struct ble_gatt_chr;

namespace grohe_ble {

// Full BLE connection lifecycle. As of M5,
// Idle -> Initializing -> Scanning -> DeviceFound -> Connecting ->
// Connected -> DiscoveringServices -> ReadyForProtocol is reachable end to
// end; kReady and kBackoff remain reserved for a future milestone (protocol
// communication and reconnect, respectively).
enum class BleState {
  kIdle,
  kInitializing,
  kScanning,
  kDeviceFound,
  kConnecting,
  kConnected,
  kDiscoveringServices,
  kReadyForProtocol,
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
    case BleState::kConnected:
      return "Connected";
    case BleState::kDiscoveringServices:
      return "DiscoveringServices";
    case BleState::kReadyForProtocol:
      return "ReadyForProtocol";
    case BleState::kReady:
      return "Ready";
    case BleState::kDisconnected:
      return "Disconnected";
    case BleState::kBackoff:
      return "Backoff";
  }
  return "";
}

// Only the events this firmware can actually produce. Protocol-level events
// (a characteristic notification, ...) belong to a future milestone, once
// there is code that can actually raise them.
enum class BleEventType {
  kHostSynced,
  kHostReset,
  kDeviceFound,
  kReadyForProtocol,
  // One shared failure event for connect-timeout, connect-failure,
  // discovery-level GATT errors, and unexpected disconnects: `reason`
  // carries the specific NimBLE/HCI status code, and the detailed narrative
  // is already in ble_manager's own log line at the point of failure. The
  // log-only consumer today doesn't need four near-identical event types to
  // tell those apart, and adding them now would be speculative.
  kConnectionFailed,
};

[[nodiscard]] constexpr const char* ToString(BleEventType type) {
  switch (type) {
    case BleEventType::kHostSynced:
      return "HostSynced";
    case BleEventType::kHostReset:
      return "HostReset";
    case BleEventType::kDeviceFound:
      return "DeviceFound";
    case BleEventType::kReadyForProtocol:
      return "ReadyForProtocol";
    case BleEventType::kConnectionFailed:
      return "ConnectionFailed";
  }
  return "";
}

struct BleEvent {
  BleEventType type;
  int reason = 0;  // Meaningful for kHostReset and kConnectionFailed.
};

// Owns the NimBLE host stack lifecycle and the BLE state machine. NimBLE's
// GAP/GATT callbacks fire on NimBLE's own host task, never on the caller's
// task -- BleManager hands them off through a bounded queue so nothing
// outside PollEvents() ever runs on that task's stack (see
// docs/ARCHITECTURE.md's threading section). Callbacks do the minimum
// possible work: build a BleEvent and enqueue it.
//
// Scope as of M5: initialize the host, actively scan until the Grohe Blue
// appliance is identified by its advertised service UUID (see
// ble_constants.hpp), connect to it, negotiate MTU, and discover every GATT
// service and characteristic it exposes. No protocol communication
// (reads/writes/notifications) and no authentication yet -- that's
// grohe_ble's next milestone.
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
  // Blue -- stop scanning, record it, and connect. Runs on the NimBLE host
  // task.
  void HandleDiscReport(const struct ble_gap_disc_desc& disc);

  // Initiates the connection to device_addr_. Called once, right after
  // HandleDiscReport() records the address.
  esp_err_t Connect();

  // GATT/MTU procedure callbacks. Same "static trampoline forwards to an
  // instance method via cb_arg" shape as OnGapEvent/HandleDiscReport.
  static int OnMtuResult(uint16_t conn_handle,
                        const struct ble_gatt_error* error, uint16_t mtu,
                        void* arg);
  static int OnSvcDisc(uint16_t conn_handle,
                       const struct ble_gatt_error* error,
                       const struct ble_gatt_svc* service, void* arg);
  static int OnChrDisc(uint16_t conn_handle,
                       const struct ble_gatt_error* error,
                       const struct ble_gatt_chr* chr, void* arg);

  void HandleConnect(const struct ble_gap_event& event);
  void HandleDisconnect(const struct ble_gap_event& event);
  void HandleSvcDisc(const struct ble_gatt_error& error,
                     const struct ble_gatt_svc* service);
  void HandleChrDisc(const struct ble_gatt_error& error,
                     const struct ble_gatt_chr* chr);

  // Starts characteristic discovery for services_[next_svc_to_disc_], then
  // advances the cursor. Called once after service discovery completes, and
  // again each time one service's characteristic discovery finishes.
  void DiscoverNextServiceChrs();

  // Logs the failure, tears down the connection if one exists, and reports
  // it through the queue. Shared by every failure path (connect
  // timeout/failure, discovery-level GATT error, unexpected disconnect).
  void FailConnection(const char* what, int reason);

  void SetState(BleState state);
  void Enqueue(const BleEvent& event);

  QueueHandle_t event_queue_ = nullptr;
  BleState state_ = BleState::kIdle;

  // The appliance's address, recorded once it is discovered. Only meaningful
  // from kDeviceFound onwards -- a host reset returns the state machine to
  // kIdle without clearing this, and the next successful discovery
  // overwrites it, so read the state before trusting it.
  //
  // Deliberately has no accessor yet: nothing outside this class needs it --
  // Connect() is the only consumer, and it lives right here. A future
  // milestone that needs reconnect will read it the same way Connect() does.
  ble_addr_t device_addr_ = {};

  // Valid once kConnected or later; BLE_HS_CONN_HANDLE_NONE otherwise.
  uint16_t conn_handle_ = 0xffff;

  // Sequencing state for "discover characteristics one service at a time",
  // and nothing more -- not a GATT cache. Reset to empty every time
  // HandleConnect() sees a fresh successful connection (including a
  // controller-level reattempt reconnecting after an earlier attempt died
  // mid-discovery -- see the reset there for why it can't live in Connect()
  // alone). Only the handle range is kept: a service's UUID is already
  // logged the instant its discovery callback fires, so storing it again
  // here would serve no purpose. A future protocol milestone decides what
  // (if anything) about the GATT hierarchy is worth persisting past this
  // discovery pass, and will add its own storage for that.
  struct ServiceHandleRange {
    uint16_t start_handle;
    uint16_t end_handle;
  };
  static constexpr size_t kMaxServices = 8;
  ServiceHandleRange services_[kMaxServices] = {};
  size_t num_services_ = 0;
  size_t next_svc_to_disc_ = 0;

  // NimBLE's ble_hs_cfg callbacks are plain C function pointers with no
  // user-data parameter, so the static callbacks above reach instance state
  // through this back-pointer -- the same pattern already used by
  // Gc9a01Display for the same reason (see gc9a01_display.hpp). Exactly one
  // BleManager instance ever exists (owned by GroheClient, owned by App),
  // matching Gc9a01Display's own lifetime.
  static BleManager* instance_;
};

}  // namespace grohe_ble
