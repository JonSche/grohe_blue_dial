#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "host/ble_uuid.h"
#include "nimble/ble.h"

// BleCharacteristicEvent below needs ble_uuid_any_t by value, so ble_uuid.h
// (lightweight -- type definitions only, no host-stack dependency) is
// included directly. The remaining NimBLE GAP/GATT types, needed only as
// callback parameters, are forward-declared so consumers of this header
// (GroheClient, and through it app::App) don't also transitively pull in
// host/ble_gap.h or host/ble_gatt.h.
struct ble_gap_event;
struct ble_gap_disc_desc;
struct ble_gatt_error;
struct ble_gatt_svc;
struct ble_gatt_chr;
struct ble_gatt_dsc;
struct ble_gatt_attr;

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

// A discovered characteristic's UUID, or one received notification/
// indication payload -- deliberately a second, separate queue/event type
// from BleEvent above, not a new BleEventType. BleEvent exists for BLE
// connection-lifecycle signals; this one carries raw GATT data for
// whichever protocol module (GroheProtocol) wants to interpret it.
// Keeping them apart means the lifecycle queue and its consumers (M3.1
// through M5) stay completely unchanged, and this struct -- which must be
// large enough to hold a full notification payload -- doesn't bloat every
// lifecycle event too.
enum class BleCharacteristicEventKind {
  kFound,
  kNotification,
};

struct BleCharacteristicEvent {
  BleCharacteristicEventKind kind;

  // Meaningful for kFound: the discovered characteristic's UUID and value
  // handle.
  ble_uuid_any_t uuid = {};
  uint16_t val_handle = 0;

  // Meaningful for kNotification: which handle the payload arrived on
  // (val_handle above is not populated for this kind -- there is nothing to
  // look up a UUID for without re-running discovery, and the handle alone
  // is exactly what the protocol layer needs to know which characteristic
  // this is), the payload bytes, and how many of them are valid.
  uint16_t notify_handle = 0;
  // 253 = the largest possible single ATT notification value at this
  // build's negotiated 256-byte MTU (MTU - 3 bytes of ATT opcode+handle
  // overhead) -- sized to hold the largest payload this connection could
  // ever receive without truncation, not just today's short protocol
  // strings.
  static constexpr size_t kMaxPayloadSize = 253;
  uint8_t payload[kMaxPayloadSize] = {};
  size_t payload_len = 0;
};

// Owns the NimBLE host stack lifecycle and the BLE state machine. NimBLE's
// GAP/GATT callbacks fire on NimBLE's own host task, never on the caller's
// task -- BleManager hands them off through a bounded queue so nothing
// outside PollEvents()/PollCharacteristicEvents() ever runs on that task's
// stack (see docs/ARCHITECTURE.md's threading section). Callbacks do the
// minimum possible work: build an event and enqueue it.
//
// Scope as of M6: initialize the host, scan, connect, negotiate MTU, and
// discover every GATT service and characteristic (all as of M5), plus:
// every discovered characteristic is reported through
// PollCharacteristicEvents(), and -- entirely internally, still on the
// host task -- BleManager recognizes the Grohe read characteristic (see
// ble_constants.hpp) among them and automatically subscribes to it, exactly
// where the Python reference implementation's start_notifications() does.
// Received notification payloads are reported the same way, as raw bytes;
// interpreting them (parsing, structured logging) is GroheProtocol's job,
// not this class's -- see grohe_protocol.hpp. This one exception to "no
// Grohe-specific knowledge in BleManager" mirrors the identical, already-
// established precedent of kGroheServiceUuid driving the scan filter: which
// peer to connect to, and which of its characteristics to subscribe to,
// are both connection-establishment policy, not protocol/payload
// interpretation, and keeping the subscribe trigger here (rather than
// having GroheClient call back into BleManager from the app task) means
// every NimBLE call this class makes still happens on the one task that
// has ever touched this class's state, with no new cross-task access to
// reason about.
//
// No writes, and no authentication, yet -- that's grohe_ble's next
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

  // Same contract as PollEvents(), for the separate characteristic/
  // notification queue (see BleCharacteristicEvent's own comment for why
  // it's a second queue rather than folded into BleEvent).
  void PollCharacteristicEvents(
      const std::function<void(const BleCharacteristicEvent&)>& on_event);

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

  // Reports every discovered characteristic on the characteristic queue,
  // and -- if it's the Grohe read characteristic -- records it as pending
  // subscribe (see pending_subscribe_val_handle_ for why the actual search
  // is deferred). Called from HandleChrDisc() for every characteristic,
  // still on the host task.
  void OnCharacteristicDiscovered(const struct ble_gatt_chr& chr);

  // Issues the deferred ble_gattc_disc_all_dscs() call for
  // pending_subscribe_val_handle_, once its search range's end handle is
  // finally known (see the field's own comment for why this is deferred).
  void StartDescriptorDiscovery(uint16_t end_handle);

  // Descriptor-discovery and CCCD-write callbacks for the subscribe
  // sequence, same "static trampoline resolves via instance_" shape as
  // every other NimBLE callback in this class.
  static int OnDscDisc(uint16_t conn_handle,
                       const struct ble_gatt_error* error,
                       uint16_t chr_val_handle,
                       const struct ble_gatt_dsc* dsc, void* arg);
  static int OnSubscribeWrite(uint16_t conn_handle,
                              const struct ble_gatt_error* error,
                              struct ble_gatt_attr* attr, void* arg);
  void HandleDscDisc(const struct ble_gatt_error& error,
                     const struct ble_gatt_dsc* dsc);
  void HandleSubscribeWrite(const struct ble_gatt_error& error);

  // BLE_GAP_EVENT_NOTIFY_RX handling: copies the payload out of the mbuf
  // (NimBLE frees it once OnGapEvent returns, same constraint M5 already
  // handled for GATT attr callbacks) and reports it on the characteristic
  // queue. Runs on the host task, like every other OnGapEvent case.
  void HandleNotifyRx(const struct ble_gap_event& event);

  void EnqueueCharacteristic(const BleCharacteristicEvent& event);

  // Logs the failure, tears down the connection if one exists, and reports
  // it through the queue. Shared by every failure path (connect
  // timeout/failure, discovery-level GATT error, unexpected disconnect,
  // subscribe failure).
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

  QueueHandle_t characteristic_queue_ = nullptr;

  // The read characteristic's value handle, non-zero from the moment it's
  // found until its subscribe sequence resolves one way or another (CCCD
  // found and write kicked off, or given up as missing). Descriptor
  // discovery for one characteristic needs an end handle strictly less
  // than the *next* characteristic's declaration handle (matching
  // ESP-IDF's own bundled blecent example, apps/blecent/src/peer.c's
  // chr_end_handle()) -- but NimBLE reports characteristics one at a time,
  // in handle order, so that boundary isn't known yet at the moment the
  // read characteristic itself is reported. The search is therefore
  // deferred: this field records "found, waiting for a boundary",
  // descriptor_search_started_ below distinguishes that from "boundary
  // known, search issued, waiting for the CCCD or EDONE". Both reset per-
  // connection alongside num_services_/next_svc_to_disc_.
  uint16_t pending_subscribe_val_handle_ = 0;
  bool descriptor_search_started_ = false;

  // Guards against starting the subscribe sequence twice for the same
  // connection (a duplicate kFound-equivalent match, however unlikely,
  // should not issue a second ble_gattc_disc_all_dscs()/write_flat() pair).
  // Reset per-connection alongside the members above.
  bool subscribe_started_ = false;

  // NimBLE's ble_hs_cfg callbacks are plain C function pointers with no
  // user-data parameter, so the static callbacks above reach instance state
  // through this back-pointer -- the same pattern already used by
  // Gc9a01Display for the same reason (see gc9a01_display.hpp). Exactly one
  // BleManager instance ever exists (owned by GroheClient, owned by App),
  // matching Gc9a01Display's own lifetime.
  static BleManager* instance_;
};

}  // namespace grohe_ble
