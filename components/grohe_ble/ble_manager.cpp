#include "grohe_ble/ble_manager.hpp"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "grohe_ble/ble_constants.hpp"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/hci_common.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"

namespace grohe_ble {
namespace {
constexpr char kTag[] = "ble_manager";
constexpr char kDeviceName[] = "grohe-dial";
// Events are rare (a handful of state changes, not a data stream), so a
// small bounded depth is generous, not tight.
constexpr UBaseType_t kEventQueueDepth = 8;
// A full discovery pass reports 5 characteristics on this appliance's own
// hierarchy (confirmed on hardware in M5), all enqueued in a short burst
// well before the app task's next 20ms poll -- sized comfortably above
// that so a fast discovery pass can never fill this queue before it's
// drained.
constexpr UBaseType_t kCharacteristicQueueDepth = 16;
// This milestone (M7) issues exactly one write per connection (GroheClient's
// stop() probe) -- depth 2 is already generous for that, matching this
// class's existing "size for the load this milestone actually has"
// precedent rather than the speculative multi-command load a future
// control milestone might add.
constexpr UBaseType_t kCommandQueueDepth = 2;

// The standard Client Characteristic Configuration Descriptor UUID
// (0x2902), used to find where to write to enable notifications. This one
// genuinely is 16-bit on the wire on this device -- confirmed directly via
// this class's own descriptor-discovery log (ble_uuid_to_str() rendered it
// as "0x2902", the short form, which only happens for BLE_UUID_TYPE_16;
// see FormatServiceUuids()/HandleDscDisc() for the same rendering rule
// applied elsewhere). Bleak/CoreBluetooth separately reports this same
// descriptor as the expanded 128-bit string
// "00002902-0000-1000-8000-00805f9b34fb" -- that's CoreBluetooth's own
// higher-level API normalizing every UUID to 128-bit *for display*,
// regardless of the actual over-the-air encoding, and is not evidence
// about wire type the way it was for kGroheReadCharUuid/kGroheWriteCharUuid
// in ble_constants.hpp (those really are 128-bit on the wire, confirmed
// independently by this same class's own characteristic-discovery log).
constexpr ble_uuid16_t kCccdUuid = BLE_UUID16_INIT(BLE_GATT_DSC_CLT_CFG_UUID16);

// Client Characteristic Configuration Descriptor value that enables
// notifications (bit 0) without indications (bit 1) -- see Bluetooth Core
// Spec, the CCCD's own definition.
constexpr uint8_t kEnableNotificationsValue[2] = {0x01, 0x00};

// Scan interval and window are set to this same value, giving a continuous
// (100% duty cycle) scan -- see StartScan().
constexpr uint32_t kScanIntervalMs = 30;

// Matches the Python reference implementation's own DEFAULT_CONNECT_TIMEOUT
// (ble.py), both for direct consistency with it and because it's a
// reasonable value independently.
constexpr int32_t kConnectTimeoutMs = 10000;

// "aa:bb:cc:dd:ee:ff" + NUL.
constexpr size_t kAddrStrSize = 18;
// Legacy advertising payloads are at most 31 bytes, so these bounds are
// never actually reached; they exist so the formatters below can't overrun
// regardless of what a peer sends.
constexpr size_t kNameBufSize = 32;
constexpr size_t kHexBufSize = 2 * 31 + 1;
constexpr size_t kUuidListBufSize = 4 * BLE_UUID_STR_LEN;
// "BROADCAST WRITE_NO_RSP INDICATE AUTH_SIGN_WRITE EXTENDED " etc. -- all 8
// property names, space-separated, generously rounded up.
constexpr size_t kPropsBufSize = 96;

// Canonical BLE address text: most-significant byte first. NimBLE stores
// addresses little-endian, hence the reversed indexing.
void FormatAddr(const ble_addr_t& addr, char* out, size_t out_size) {
  snprintf(out, out_size, "%02x:%02x:%02x:%02x:%02x:%02x", addr.val[5],
           addr.val[4], addr.val[3], addr.val[2], addr.val[1], addr.val[0]);
}

[[nodiscard]] const char* AddrTypeToString(uint8_t type) {
  switch (type) {
    case BLE_ADDR_PUBLIC:
      return "public";
    case BLE_ADDR_RANDOM:
      return "random";
    case BLE_ADDR_PUBLIC_ID:
      return "public-id";
    case BLE_ADDR_RANDOM_ID:
      return "random-id";
    default:
      return "unknown";
  }
}

[[nodiscard]] const char* AdvTypeToString(uint8_t event_type) {
  switch (event_type) {
    case BLE_HCI_ADV_RPT_EVTYPE_ADV_IND:
      return "ADV_IND";
    case BLE_HCI_ADV_RPT_EVTYPE_DIR_IND:
      return "DIR_IND";
    case BLE_HCI_ADV_RPT_EVTYPE_SCAN_IND:
      return "SCAN_IND";
    case BLE_HCI_ADV_RPT_EVTYPE_NONCONN_IND:
      return "NONCONN_IND";
    case BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP:
      return "SCAN_RSP";
    default:
      return "unknown";
  }
}

void FormatHex(const uint8_t* data, uint8_t len, char* out, size_t out_size) {
  static constexpr char kHexDigits[] = "0123456789abcdef";
  size_t pos = 0;
  // pos + 2 < out_size leaves room for both nibbles and the terminator.
  for (uint8_t i = 0; i < len && pos + 2 < out_size; ++i) {
    out[pos++] = kHexDigits[data[i] >> 4];
    out[pos++] = kHexDigits[data[i] & 0x0f];
  }
  out[pos] = '\0';
}

// The local name in an advertisement is not NUL-terminated -- it's a pointer
// into the payload plus a length -- so it has to be copied out before it can
// be logged as a string.
void FormatName(const ble_hs_adv_fields& fields, char* out, size_t out_size) {
  if (fields.name == nullptr || fields.name_len == 0) {
    out[0] = '\0';
    return;
  }
  const size_t len = fields.name_len < out_size - 1 ? fields.name_len
                                                     : out_size - 1;
  std::memcpy(out, fields.name, len);
  out[len] = '\0';
}

// Every advertised service UUID (16-, 32- and 128-bit), space-separated.
void FormatServiceUuids(const ble_hs_adv_fields& fields, char* out,
                        size_t out_size) {
  size_t pos = 0;
  const auto append = [&](const ble_uuid_t* uuid) {
    char buf[BLE_UUID_STR_LEN];
    ble_uuid_to_str(uuid, buf);
    const size_t len = std::strlen(buf);
    const size_t separator = pos > 0 ? 1 : 0;
    if (pos + separator + len + 1 > out_size) {
      return;  // Keep whatever already fits rather than truncating mid-UUID.
    }
    if (separator != 0) {
      out[pos++] = ' ';
    }
    std::memcpy(out + pos, buf, len);
    pos += len;
  };

  for (uint8_t i = 0; i < fields.num_uuids16; ++i) {
    append(&fields.uuids16[i].u);
  }
  for (uint8_t i = 0; i < fields.num_uuids32; ++i) {
    append(&fields.uuids32[i].u);
  }
  for (uint8_t i = 0; i < fields.num_uuids128; ++i) {
    append(&fields.uuids128[i].u);
  }
  out[pos] = '\0';
}

// Characteristic property flags (BLE_GATT_CHR_PROP_*) as readable names,
// space-separated, rather than a raw hex byte.
void FormatChrProperties(uint8_t properties, char* out, size_t out_size) {
  struct Flag {
    uint8_t bit;
    const char* name;
  };
  static constexpr Flag kFlags[] = {
      {BLE_GATT_CHR_PROP_BROADCAST, "BROADCAST"},
      {BLE_GATT_CHR_PROP_READ, "READ"},
      {BLE_GATT_CHR_PROP_WRITE_NO_RSP, "WRITE_NO_RSP"},
      {BLE_GATT_CHR_PROP_WRITE, "WRITE"},
      {BLE_GATT_CHR_PROP_NOTIFY, "NOTIFY"},
      {BLE_GATT_CHR_PROP_INDICATE, "INDICATE"},
      {BLE_GATT_CHR_PROP_AUTH_SIGN_WRITE, "AUTH_SIGN_WRITE"},
      {BLE_GATT_CHR_PROP_EXTENDED, "EXTENDED"},
  };
  size_t pos = 0;
  for (const auto& flag : kFlags) {
    if ((properties & flag.bit) == 0) {
      continue;
    }
    const size_t len = std::strlen(flag.name);
    const size_t separator = pos > 0 ? 1 : 0;
    if (pos + separator + len + 1 > out_size) {
      break;
    }
    if (separator != 0) {
      out[pos++] = ' ';
    }
    std::memcpy(out + pos, flag.name, len);
    pos += len;
  }
  out[pos] = '\0';
}

// The detection predicate, ported from the Python reference implementation:
// the appliance is whatever advertises the Grohe service UUID. Only the
// 128-bit list is searched because that's the width of the UUID --
// ble_uuid_cmp() compares type before value, so a 16- or 32-bit entry could
// never match anyway.
[[nodiscard]] bool AdvertisesGroheService(const ble_hs_adv_fields& fields) {
  for (uint8_t i = 0; i < fields.num_uuids128; ++i) {
    if (ble_uuid_cmp(&fields.uuids128[i].u, &kGroheServiceUuid.u) == 0) {
      return true;
    }
  }
  return false;
}
}  // namespace

BleManager* BleManager::instance_ = nullptr;

BleManager::~BleManager() {
  // NimBLE's own clean shutdown (nimble_port_stop() + task join +
  // nimble_port_deinit()) is not implemented here: like every other
  // subsystem in this codebase (see Gc9a01Display's own destructor note),
  // BleManager is owned for the process lifetime and this path is never
  // actually exercised. Only the queue -- which this class unconditionally
  // owns -- is freed.
  if (event_queue_ != nullptr) {
    vQueueDelete(event_queue_);
    event_queue_ = nullptr;
  }
  if (characteristic_queue_ != nullptr) {
    vQueueDelete(characteristic_queue_);
    characteristic_queue_ = nullptr;
  }
  if (command_queue_ != nullptr) {
    vQueueDelete(command_queue_);
    command_queue_ = nullptr;
  }
  if (instance_ == this) {
    instance_ = nullptr;
  }
}

esp_err_t BleManager::Init() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    err = nvs_flash_erase();
    if (err != ESP_OK) {
      ESP_LOGE(kTag, "nvs_flash_erase failed: %s", esp_err_to_name(err));
      return err;
    }
    err = nvs_flash_init();
  }
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "nvs_flash_init failed: %s", esp_err_to_name(err));
    return err;
  }

  event_queue_ = xQueueCreate(kEventQueueDepth, sizeof(BleEvent));
  if (event_queue_ == nullptr) {
    ESP_LOGE(kTag, "xQueueCreate failed");
    return ESP_ERR_NO_MEM;
  }

  characteristic_queue_ = xQueueCreate(kCharacteristicQueueDepth,
                                       sizeof(BleCharacteristicEvent));
  if (characteristic_queue_ == nullptr) {
    ESP_LOGE(kTag, "xQueueCreate (characteristic) failed");
    return ESP_ERR_NO_MEM;
  }

  command_queue_ =
      xQueueCreate(kCommandQueueDepth, sizeof(BleWriteCommand));
  if (command_queue_ == nullptr) {
    ESP_LOGE(kTag, "xQueueCreate (command) failed");
    return ESP_ERR_NO_MEM;
  }

  instance_ = this;
  SetState(BleState::kInitializing);

  err = nimble_port_init();
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "nimble_port_init failed: %s", esp_err_to_name(err));
    return err;
  }

  // ble_npl_event_init() draws from an NPL memory pool that nimble_port_init()
  // itself sets up -- calling this any earlier (confirmed on hardware: a
  // Guru Meditation Load access fault, before any ble_manager log line)
  // reads that pool before it exists. Every other NimBLE call in this
  // function already happens after nimble_port_init() for the same reason;
  // this one just wasn't obviously a NimBLE call by name.
  ble_npl_event_init(&command_event_, &BleManager::OnCommandEvent, this);

  ble_hs_cfg.reset_cb = &BleManager::OnHostReset;
  ble_hs_cfg.sync_cb = &BleManager::OnHostSync;

  const int rc = ble_svc_gap_device_name_set(kDeviceName);
  if (rc != 0) {
    ESP_LOGE(kTag, "ble_svc_gap_device_name_set failed: %d", rc);
    return ESP_FAIL;
  }

  nimble_port_freertos_init(&BleManager::HostTask);

  ESP_LOGI(kTag, "NimBLE host starting");
  return ESP_OK;
}

BleState BleManager::State() const { return state_; }

void BleManager::PollEvents(const std::function<void(const BleEvent&)>& on_event) {
  if (event_queue_ == nullptr) {
    return;
  }
  BleEvent event;
  while (xQueueReceive(event_queue_, &event, 0) == pdTRUE) {
    on_event(event);
  }
}

void BleManager::PollCharacteristicEvents(
    const std::function<void(const BleCharacteristicEvent&)>& on_event) {
  if (characteristic_queue_ == nullptr) {
    return;
  }
  BleCharacteristicEvent event;
  while (xQueueReceive(characteristic_queue_, &event, 0) == pdTRUE) {
    on_event(event);
  }
}

esp_err_t BleManager::WriteCharacteristic(uint16_t val_handle,
                                          const uint8_t* data,
                                          size_t data_len) {
  if (val_handle == 0 || data_len > BleWriteCommand::kMaxPayloadSize) {
    return ESP_ERR_INVALID_ARG;
  }
  if (command_queue_ == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }

  BleWriteCommand cmd;
  cmd.val_handle = val_handle;
  std::memcpy(cmd.payload, data, data_len);
  cmd.payload_len = data_len;

  if (xQueueSend(command_queue_, &cmd, 0) != pdTRUE) {
    ESP_LOGW(kTag, "command queue full, dropping write to handle=%u",
             val_handle);
    return ESP_ERR_INVALID_STATE;
  }
  // Wakes the host task's nimble_port_run() loop to actually process the
  // command -- see the class comment in ble_manager.hpp for why this, and
  // not a synchronized conn_handle_, is how the write reaches the host
  // task.
  ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &command_event_);
  return ESP_OK;
}

void BleManager::SetState(BleState state) {
  ESP_LOGI(kTag, "state: %s -> %s", ToString(state_), ToString(state));
  state_ = state;
}

void BleManager::Enqueue(const BleEvent& event) {
  if (event_queue_ == nullptr) {
    return;
  }
  if (xQueueSend(event_queue_, &event, 0) != pdTRUE) {
    ESP_LOGW(kTag, "event queue full, dropping %s", ToString(event.type));
  }
}

void BleManager::EnqueueCharacteristic(const BleCharacteristicEvent& event) {
  if (characteristic_queue_ == nullptr) {
    return;
  }
  if (xQueueSend(characteristic_queue_, &event, 0) != pdTRUE) {
    ESP_LOGW(kTag, "characteristic queue full, dropping event");
  }
}

void BleManager::HostTask(void* /*param*/) {
  ESP_LOGI(kTag, "NimBLE host task started");
  // Returns only after nimble_port_stop() -- never, in practice, per the
  // destructor note above.
  nimble_port_run();
  nimble_port_freertos_deinit();
}

void BleManager::OnHostSync() {
  const int rc = ble_hs_util_ensure_addr(0);
  if (rc != 0) {
    ESP_LOGE(kTag, "ble_hs_util_ensure_addr failed: %d", rc);
    return;
  }
  ESP_LOGI(kTag, "NimBLE host synced");
  if (instance_ == nullptr) {
    return;
  }
  instance_->Enqueue(BleEvent{BleEventType::kHostSynced});
  if (instance_->StartScan() != ESP_OK) {
    // StartScan() logs the specific failure. There is no retry/backoff yet
    // (that milestone hasn't landed), so the state machine deliberately
    // stays where it is rather than pretending to scan.
    ESP_LOGE(kTag, "scan did not start; BLE will stay idle until a host reset");
  }
}

esp_err_t BleManager::StartScan() {
  uint8_t own_addr_type = 0;
  int rc = ble_hs_id_infer_auto(0, &own_addr_type);
  if (rc != 0) {
    ESP_LOGE(kTag, "ble_hs_id_infer_auto failed: %d", rc);
    return ESP_FAIL;
  }

  ble_gap_disc_params params = {};
  // Window == interval, i.e. scan continuously with no gaps. The appliance
  // can be near the receiver's sensitivity limit, where it may only be heard
  // once in tens of seconds, so every missed advertising event directly costs
  // discovery latency. NimBLE's own defaults (30ms/30ms) happen to be
  // continuous today, but they're set explicitly here so this milestone's
  // core guarantee doesn't rest on an upstream default staying that way.
  params.itvl = BLE_GAP_SCAN_ITVL_MS(kScanIntervalMs);
  params.window = BLE_GAP_SCAN_WIN_MS(kScanIntervalMs);
  params.filter_policy = 0;
  params.limited = 0;
  // Active scan: send SCAN_REQ so peers answer with their scan response.
  params.passive = 0;
  // Duplicate filtering stays OFF deliberately. This build's controller
  // filters on address only (CONFIG_BT_CTRL_SCAN_DUPL_TYPE_DEVICE), so
  // enabling it would suppress a device's SCAN_RSP once its ADV_IND had been
  // seen -- and a 128-bit service UUID costs 17 of the 31 available payload
  // bytes, so peers commonly carry it in the scan response rather than the
  // advertisement. Filtering here could therefore hide the very field the
  // detection predicate matches on. The cost is repeated reports from
  // nearby devices, which stop as soon as the appliance is found.
  params.filter_duplicates = 0;

  // BLE_HS_FOREVER: finding the appliance is this milestone's whole job, and
  // there is no timeout handler to hand off to yet.
  rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &params,
                    &BleManager::OnGapEvent, this);
  if (rc != 0) {
    ESP_LOGE(kTag, "ble_gap_disc failed: %d", rc);
    return ESP_FAIL;
  }

  SetState(BleState::kScanning);

  char uuid_str[BLE_UUID_STR_LEN];
  ble_uuid_to_str(&kGroheServiceUuid.u, uuid_str);
  ESP_LOGI(kTag, "Active scan started; looking for service %s", uuid_str);
  return ESP_OK;
}

int BleManager::OnGapEvent(struct ble_gap_event* event, void* /*arg*/) {
  // Deliberately ignores `arg`/cb_arg and uses instance_ instead -- see the
  // long comment on instance_ in ble_manager.hpp. Confirmed via hardware
  // testing and reading ESP-IDF's bundled NimBLE source
  // (nimble/host/src/ble_gap.c's ble_gap_master_connect_reattempt(), guarded
  // by MYNEWT_VAL(BLE_ENABLE_CONN_REATTEMPT)): when a connection attempt to
  // a weak-signal peer fails at the link layer, the controller silently
  // reattempts it, and that internal reattempt path re-invokes this same
  // callback with cb_arg pointing at a *local stack variable* from the
  // reattempt function -- not the original cb_arg. Trusting cb_arg there
  // reads/writes through a dangling pointer once that function has
  // returned, and reproducibly showed up on real hardware as garbage state_
  // values (e.g. a logged transition through a state name that doesn't
  // exist). Scanning's own ble_gap_disc() callback doesn't go through this
  // path and would have a valid arg, but the two share this one function,
  // so the safe rule is: never trust arg here, always resolve through
  // instance_, exactly like OnHostSync()/OnHostReset() already do.
  auto* self = instance_;
  if (self == nullptr || event == nullptr) {
    return 0;
  }

  switch (event->type) {
    case BLE_GAP_EVENT_DISC:
      self->HandleDiscReport(event->disc);
      break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
      // Only reached if the scan ends on its own (host reset, or a duration
      // expiring). ble_gap_disc_cancel() does not raise this event.
      ESP_LOGI(kTag, "scan ended; reason=%d", event->disc_complete.reason);
      break;
    case BLE_GAP_EVENT_CONNECT:
      self->HandleConnect(*event);
      break;
    case BLE_GAP_EVENT_DISCONNECT:
      self->HandleDisconnect(*event);
      break;
    case BLE_GAP_EVENT_NOTIFY_RX:
      self->HandleNotifyRx(*event);
      break;
    case BLE_GAP_EVENT_MTU:
      // Informational only: the MTU exchange this code cares about is the
      // one explicitly started in HandleConnect(), whose result arrives via
      // OnMtuResult() and is what actually advances the state machine. This
      // GAP-level event fires for the same exchange (and would also fire
      // for one a peer initiated, which this appliance does not), so it's
      // logged at debug level rather than acted on twice.
      ESP_LOGD(kTag, "MTU event: conn_handle=%d value=%d",
               event->mtu.conn_handle, event->mtu.value);
      break;
    default:
      ESP_LOGD(kTag, "unhandled GAP event: type=%d", event->type);
      break;
  }
  return 0;
}

void BleManager::HandleDiscReport(const struct ble_gap_disc_desc& disc) {
  // Reports the host had already queued when the scan was cancelled can
  // still be delivered, and a cancel that fails leaves the scan running, so
  // this handler has to be idempotent: once the appliance is found, ignore
  // everything that follows.
  if (state_ != BleState::kScanning) {
    return;
  }

  ble_hs_adv_fields fields = {};
  const int rc =
      ble_hs_adv_parse_fields(&fields, disc.data, disc.length_data);
  if (rc != 0) {
    // A malformed or truncated payload is a property of the peer, not a
    // local fault -- log it and keep scanning.
    ESP_LOGW(kTag, "ble_hs_adv_parse_fields failed: %d", rc);
    return;
  }

  char addr_str[kAddrStrSize];
  FormatAddr(disc.addr, addr_str, sizeof(addr_str));
  char name[kNameBufSize];
  FormatName(fields, name, sizeof(name));
  char uuids[kUuidListBufSize];
  FormatServiceUuids(fields, uuids, sizeof(uuids));
  char mfg[kHexBufSize];
  FormatHex(fields.mfg_data, fields.mfg_data_len, mfg, sizeof(mfg));
  // 128-bit service *data* (AD type 0x21) is a different field from the
  // service class UUID lists the detection predicate matches on. It is
  // logged purely so that a peer advertising the Grohe UUID this way would
  // still be visible here, rather than showing up as an empty uuids=[] with
  // no explanation.
  char svc_data[kHexBufSize];
  FormatHex(fields.svc_data_uuid128, fields.svc_data_uuid128_len, svc_data,
            sizeof(svc_data));

  ESP_LOGI(kTag,
           "adv addr=%s type=%s rssi=%d pdu=%s name='%s' uuids=[%s] mfg=%s "
           "svcdata128=%s",
           addr_str, AddrTypeToString(disc.addr.type), disc.rssi,
           AdvTypeToString(disc.event_type), name, uuids,
           mfg[0] != '\0' ? mfg : "-",
           svc_data[0] != '\0' ? svc_data : "-");

  if (!AdvertisesGroheService(fields)) {
    return;
  }

  // Stop scanning before anything else, so the window in which further
  // reports can arrive is as short as possible.
  const int cancel_rc = ble_gap_disc_cancel();
  if (cancel_rc != 0 && cancel_rc != BLE_HS_EALREADY) {
    // Worth knowing about, but not a reason to discard a successful
    // discovery: the state change below stops us acting on later reports
    // either way.
    ESP_LOGW(kTag, "ble_gap_disc_cancel failed: %d", cancel_rc);
  }

  device_addr_ = disc.addr;
  SetState(BleState::kDeviceFound);

  // Logged from the stored copy, not from `disc`, so the log doubles as
  // evidence that the address was recorded correctly.
  char found_addr[kAddrStrSize];
  FormatAddr(device_addr_, found_addr, sizeof(found_addr));
  ESP_LOGI(kTag, "Grohe Blue discovered: addr=%s (%s) rssi=%d name='%s'",
           found_addr, AddrTypeToString(device_addr_.type), disc.rssi, name);

  Enqueue(BleEvent{BleEventType::kDeviceFound});

  if (Connect() != ESP_OK) {
    // Connect() has already logged the specific failure and called
    // FailConnection(), which returns the state machine to kDeviceFound --
    // the address is still valid, ready for a future retry milestone.
  }
}

esp_err_t BleManager::Connect() {
  uint8_t own_addr_type = 0;
  int rc = ble_hs_id_infer_auto(0, &own_addr_type);
  if (rc != 0) {
    ESP_LOGE(kTag, "ble_hs_id_infer_auto failed: %d", rc);
    FailConnection("ble_hs_id_infer_auto", rc);
    return ESP_FAIL;
  }

  rc = ble_gap_connect(own_addr_type, &device_addr_, kConnectTimeoutMs,
                       nullptr /* default connection parameters */,
                       &BleManager::OnGapEvent, this);
  if (rc != 0) {
    ESP_LOGE(kTag, "ble_gap_connect failed: %d", rc);
    FailConnection("ble_gap_connect", rc);
    return ESP_FAIL;
  }

  SetState(BleState::kConnecting);
  char addr_str[kAddrStrSize];
  FormatAddr(device_addr_, addr_str, sizeof(addr_str));
  ESP_LOGI(kTag, "Connecting to %s (timeout %ld ms)", addr_str,
           static_cast<long>(kConnectTimeoutMs));
  return ESP_OK;
}

void BleManager::HandleConnect(const struct ble_gap_event& event) {
  if (event.connect.status != 0) {
    FailConnection("connect", event.connect.status);
    return;
  }

  conn_handle_ = event.connect.conn_handle;
  // Reset here, not just once in Connect(): the ESP32-C3 BT controller
  // autonomously reattempts a connection that fails right after being
  // established (visible on hardware as "NimBLE: Reattempt connection;
  // reason = 0x3e"), and each successful reattempt re-fires this same
  // BLE_GAP_EVENT_CONNECT, restarting MTU negotiation and discovery from
  // scratch. Resetting only in Connect() would leave stale entries from an
  // earlier, abandoned attempt sitting in services_ when a later attempt's
  // discovery starts appending to it again.
  num_services_ = 0;
  next_svc_to_disc_ = 0;
  pending_subscribe_val_handle_ = 0;
  descriptor_search_started_ = false;
  subscribe_started_ = false;
  SetState(BleState::kConnected);
  ESP_LOGI(kTag, "Connected; conn_handle=%d", conn_handle_);

  const int rc =
      ble_gattc_exchange_mtu(conn_handle_, &BleManager::OnMtuResult, this);
  if (rc != 0) {
    // Not every peer supports the exchange; discovery still works at the
    // default 23-byte MTU, just chattier. Log and proceed rather than
    // treating this as fatal.
    ESP_LOGW(kTag, "ble_gattc_exchange_mtu failed: %d; continuing at default MTU",
             rc);
    const int disc_rc =
        ble_gattc_disc_all_svcs(conn_handle_, &BleManager::OnSvcDisc, this);
    if (disc_rc != 0) {
      FailConnection("ble_gattc_disc_all_svcs", disc_rc);
      return;
    }
    SetState(BleState::kDiscoveringServices);
  }
}

int BleManager::OnMtuResult(uint16_t conn_handle,
                            const struct ble_gatt_error* error, uint16_t mtu,
                            void* /*arg*/) {
  // Resolved via instance_, not arg -- see the comment in OnGapEvent().
  auto* self = instance_;
  if (self == nullptr) {
    return 0;
  }
  if (conn_handle != self->conn_handle_) {
    // Belongs to a connection this code has already moved on from -- see
    // the comment on the same check in OnSvcDisc()/OnChrDisc() below.
    ESP_LOGD(kTag, "ignoring stale MTU result for conn_handle=%d", conn_handle);
    return 0;
  }

  if (error != nullptr && error->status != 0) {
    // Same reasoning as the ble_gattc_exchange_mtu() call-failure path in
    // HandleConnect(): non-fatal, proceed at the default MTU.
    ESP_LOGW(kTag, "MTU exchange failed: status=%d; continuing at default MTU",
             error->status);
  } else {
    ESP_LOGI(kTag, "MTU negotiated: %d", mtu);
  }

  const int rc = ble_gattc_disc_all_svcs(self->conn_handle_,
                                        &BleManager::OnSvcDisc, self);
  if (rc != 0) {
    self->FailConnection("ble_gattc_disc_all_svcs", rc);
    return 0;
  }
  self->SetState(BleState::kDiscoveringServices);
  return 0;
}

int BleManager::OnSvcDisc(uint16_t conn_handle,
                          const struct ble_gatt_error* error,
                          const struct ble_gatt_svc* service, void* /*arg*/) {
  // Resolved via instance_, not arg -- see the comment in OnGapEvent().
  auto* self = instance_;
  if (self == nullptr || error == nullptr) {
    return 0;
  }
  if (conn_handle != self->conn_handle_) {
    // The ESP32-C3 controller can reattempt a connection that dies right
    // after being established (see HandleConnect()'s comment); when it
    // does, this handle range's service discovery was started against a
    // connection that is now gone, and conn_handle_ has already moved on
    // to a newer one. A late result for the old handle -- success or
    // BLE_HS_ENOTCONN, either is possible -- must not be allowed to touch
    // state that belongs to the current connection, so it's ignored
    // entirely rather than fed into HandleSvcDisc().
    ESP_LOGD(kTag, "ignoring stale service-discovery result for conn_handle=%d",
             conn_handle);
    return 0;
  }
  self->HandleSvcDisc(*error, service);
  return 0;
}

void BleManager::HandleSvcDisc(const struct ble_gatt_error& error,
                               const struct ble_gatt_svc* service) {
  if (error.status == BLE_HS_EDONE) {
    ESP_LOGI(kTag, "Service discovery complete: %u service(s)",
             static_cast<unsigned>(num_services_));
    DiscoverNextServiceChrs();
    return;
  }
  if (error.status != 0) {
    FailConnection("service discovery", error.status);
    return;
  }
  if (service == nullptr) {
    return;
  }

  char uuid_str[BLE_UUID_STR_LEN];
  ble_uuid_to_str(&service->uuid.u, uuid_str);
  ESP_LOGI(kTag, "Service: uuid=%s handles=[%u, %u]", uuid_str,
           service->start_handle, service->end_handle);

  if (num_services_ >= kMaxServices) {
    // This appliance's own GATT hierarchy is small (confirmed against the
    // Python reference's known characteristic set); kMaxServices is sized
    // generously above that. If a peer genuinely exposes more, this drops
    // the excess from characteristic discovery -- everything is still
    // logged above -- rather than overflowing a fixed buffer.
    ESP_LOGW(kTag, "more than %u services; excess will not be characteristic-discovered",
             static_cast<unsigned>(kMaxServices));
    return;
  }
  services_[num_services_].start_handle = service->start_handle;
  services_[num_services_].end_handle = service->end_handle;
  ++num_services_;
}

void BleManager::DiscoverNextServiceChrs() {
  if (next_svc_to_disc_ >= num_services_) {
    SetState(BleState::kReadyForProtocol);
    ESP_LOGI(kTag, "GATT discovery complete; ready for protocol");
    Enqueue(BleEvent{BleEventType::kReadyForProtocol});
    return;
  }

  const ServiceHandleRange& range = services_[next_svc_to_disc_];
  const int rc = ble_gattc_disc_all_chrs(conn_handle_, range.start_handle,
                                        range.end_handle,
                                        &BleManager::OnChrDisc, this);
  if (rc != 0) {
    FailConnection("ble_gattc_disc_all_chrs", rc);
    return;
  }
}

int BleManager::OnChrDisc(uint16_t conn_handle,
                          const struct ble_gatt_error* error,
                          const struct ble_gatt_chr* chr, void* /*arg*/) {
  // Resolved via instance_, not arg -- see the comment in OnGapEvent().
  auto* self = instance_;
  if (self == nullptr || error == nullptr) {
    return 0;
  }
  if (conn_handle != self->conn_handle_) {
    // Stale result for a connection this code has already moved on from --
    // see the identical check in OnSvcDisc().
    ESP_LOGD(kTag,
             "ignoring stale characteristic-discovery result for conn_handle=%d",
             conn_handle);
    return 0;
  }
  self->HandleChrDisc(*error, chr);
  return 0;
}

void BleManager::HandleChrDisc(const struct ble_gatt_error& error,
                               const struct ble_gatt_chr* chr) {
  if (error.status == BLE_HS_EDONE) {
    // If the read characteristic was the last one in this service, no
    // subsequent characteristic ever arrived to bound its descriptor
    // search (see OnCharacteristicDiscovered()) -- the service's own end
    // handle is the correct, final boundary in that case, exactly matching
    // blecent's own chr_end_handle() for a characteristic with no
    // successor.
    if (pending_subscribe_val_handle_ != 0 && !descriptor_search_started_) {
      StartDescriptorDiscovery(services_[next_svc_to_disc_].end_handle);
    }
    // Done with this service; move on to the next one (or, if this was the
    // last one, DiscoverNextServiceChrs() transitions to kReadyForProtocol).
    ++next_svc_to_disc_;
    DiscoverNextServiceChrs();
    return;
  }
  if (error.status != 0) {
    FailConnection("characteristic discovery", error.status);
    return;
  }
  if (chr == nullptr) {
    return;
  }

  char uuid_str[BLE_UUID_STR_LEN];
  ble_uuid_to_str(&chr->uuid.u, uuid_str);
  char props[kPropsBufSize];
  FormatChrProperties(chr->properties, props, sizeof(props));
  ESP_LOGI(kTag,
           "  Characteristic: uuid=%s def_handle=%u val_handle=%u "
           "properties=[%s]",
           uuid_str, chr->def_handle, chr->val_handle, props);

  OnCharacteristicDiscovered(*chr);
}

void BleManager::OnCharacteristicDiscovered(const struct ble_gatt_chr& chr) {
  // If a previous characteristic in this same service was the read
  // characteristic and is still waiting for its search boundary, this
  // characteristic's own declaration handle is exactly that boundary --
  // descriptors belong to the *previous* characteristic up to (but not
  // including) the next one's declaration, matching blecent's own
  // chr_end_handle(). This must run before the "is chr itself the read
  // characteristic" check below, since a characteristic can't bound its
  // own search.
  if (pending_subscribe_val_handle_ != 0 && !descriptor_search_started_) {
    StartDescriptorDiscovery(chr.def_handle - 1);
  }

  BleCharacteristicEvent found_event;
  found_event.kind = BleCharacteristicEventKind::kFound;
  found_event.uuid = chr.uuid;
  found_event.val_handle = chr.val_handle;
  EnqueueCharacteristic(found_event);

  // Which characteristic to auto-subscribe to is the one piece of Grohe-
  // specific knowledge in this class -- see the class-level comment on why
  // that's an intentional, narrow exception, mirroring kGroheServiceUuid's
  // own role in the scan filter.
  if (subscribe_started_ || ble_uuid_cmp(&chr.uuid.u, &kGroheReadCharUuid.u) != 0) {
    return;
  }

  subscribe_started_ = true;
  pending_subscribe_val_handle_ = chr.val_handle;
  descriptor_search_started_ = false;
  // The search itself is deferred to StartDescriptorDiscovery(), called
  // either above (once the next characteristic in this service is
  // reported) or from HandleChrDisc()'s EDONE branch (if this was the last
  // one) -- see pending_subscribe_val_handle_'s own comment for why the
  // boundary isn't known yet at this point.
}

void BleManager::StartDescriptorDiscovery(uint16_t end_handle) {
  descriptor_search_started_ = true;
  // Search starts *at* val_handle, not val_handle + 1: confirmed against
  // both ESP-IDF's own blecent example (apps/blecent/src/peer.c's
  // peer_disc_dscs()) and, empirically, against this exact characteristic
  // on real hardware -- a search starting at val_handle + 1 consistently
  // (reproduced across multiple hardware trials) failed to find a CCCD
  // that a direct Python/CoreBluetooth query confirms does exist at
  // val_handle + 1. The characteristic's own value attribute is
  // necessarily included and returned as the first result, matching its
  // own UUID -- HandleDscDisc()'s CCCD comparison below simply doesn't
  // match it and moves on, exactly as it does for every other non-CCCD
  // descriptor.
  const int rc =
      ble_gattc_disc_all_dscs(conn_handle_, pending_subscribe_val_handle_,
                              end_handle, &BleManager::OnDscDisc, this);
  if (rc != 0) {
    FailConnection("ble_gattc_disc_all_dscs", rc);
  }
}

int BleManager::OnDscDisc(uint16_t conn_handle,
                          const struct ble_gatt_error* error,
                          uint16_t /*chr_val_handle*/,
                          const struct ble_gatt_dsc* dsc, void* /*arg*/) {
  // Resolved via instance_, not arg -- see the comment in OnGapEvent().
  auto* self = instance_;
  if (self == nullptr || error == nullptr) {
    return 0;
  }
  if (conn_handle != self->conn_handle_) {
    // Stale result for a connection this code has already moved on from --
    // see the identical check in OnSvcDisc().
    ESP_LOGD(kTag, "ignoring stale descriptor-discovery result for conn_handle=%d",
             conn_handle);
    return 0;
  }
  self->HandleDscDisc(*error, dsc);
  return 0;
}

void BleManager::HandleDscDisc(const struct ble_gatt_error& error,
                               const struct ble_gatt_dsc* dsc) {
  if (error.status == BLE_HS_EDONE) {
    if (pending_subscribe_val_handle_ != 0) {
      // All of this characteristic's descriptors were enumerated and none
      // was the CCCD -- it doesn't support what subscribing requires,
      // despite advertising the NOTIFY property. A genuine, reportable
      // mismatch between what discovery found and what this milestone
      // needs, not a transport hiccup to retry.
      FailConnection("missing CCCD for read characteristic", error.status);
    }
    return;
  }
  if (error.status != 0) {
    FailConnection("descriptor discovery", error.status);
    return;
  }
  if (dsc == nullptr || pending_subscribe_val_handle_ == 0) {
    // pending_subscribe_val_handle_ == 0 means the CCCD was already found
    // and the write already kicked off (see below) -- any further
    // descriptor in this same enumeration is simply ignored.
    return;
  }

  char dsc_uuid_str[BLE_UUID_STR_LEN];
  ble_uuid_to_str(&dsc->uuid.u, dsc_uuid_str);
  ESP_LOGI(kTag, "  Descriptor: uuid=%s handle=%u", dsc_uuid_str, dsc->handle);

  if (ble_uuid_cmp(&dsc->uuid.u, &kCccdUuid.u) != 0) {
    return;
  }

  const uint16_t chr_val_handle = pending_subscribe_val_handle_;
  pending_subscribe_val_handle_ = 0;  // Found it; stop waiting for EDONE.
  const int rc = ble_gattc_write_flat(
      conn_handle_, dsc->handle, kEnableNotificationsValue,
      sizeof(kEnableNotificationsValue), &BleManager::OnSubscribeWrite, this);
  if (rc != 0) {
    FailConnection("ble_gattc_write_flat (subscribe)", rc);
    return;
  }
  ESP_LOGI(kTag,
           "Subscribing to notifications: chr_val_handle=%u cccd_handle=%u",
           chr_val_handle, dsc->handle);
}

int BleManager::OnSubscribeWrite(uint16_t conn_handle,
                                 const struct ble_gatt_error* error,
                                 struct ble_gatt_attr* /*attr*/,
                                 void* /*arg*/) {
  // Resolved via instance_, not arg -- see the comment in OnGapEvent().
  auto* self = instance_;
  if (self == nullptr || error == nullptr) {
    return 0;
  }
  if (conn_handle != self->conn_handle_) {
    // Stale result for a connection this code has already moved on from --
    // see the identical check in OnSvcDisc().
    ESP_LOGD(kTag, "ignoring stale subscribe-write result for conn_handle=%d",
             conn_handle);
    return 0;
  }
  self->HandleSubscribeWrite(*error);
  return 0;
}

void BleManager::HandleSubscribeWrite(const struct ble_gatt_error& error) {
  if (error.status != 0) {
    FailConnection("subscribe write", error.status);
    return;
  }
  ESP_LOGI(kTag, "Subscribed to notifications successfully");
  // M7: the only way the app task can learn notifications are enabled --
  // see BleEventType::kSubscribed's own comment for why this can't just be
  // inferred from kReadyForProtocol.
  Enqueue(BleEvent{BleEventType::kSubscribed});
}

void BleManager::OnCommandEvent(struct ble_npl_event* event) {
  // Resolved via instance_, not the event's own arg -- see the comment in
  // OnGapEvent(). This arg is one this class sets itself (not an upstream
  // NimBLE reattempt path), but resolving the same way everywhere keeps
  // this file's callbacks uniform.
  auto* self = instance_;
  if (self == nullptr) {
    return;
  }
  (void)event;
  self->HandleCommandEvent();
}

void BleManager::HandleCommandEvent() {
  BleWriteCommand cmd;
  while (xQueueReceive(command_queue_, &cmd, 0) == pdTRUE) {
    if (conn_handle_ == BLE_HS_CONN_HANDLE_NONE) {
      // The connection ended between WriteCharacteristic() enqueuing this
      // and the host task getting to it -- not a failure to report, since
      // there is nothing left to report it to (FailConnection() has
      // already run for whatever this connection was).
      ESP_LOGD(kTag, "dropping queued write, no connection");
      continue;
    }
    const int rc =
        ble_gattc_write_flat(conn_handle_, cmd.val_handle, cmd.payload,
                             cmd.payload_len, &BleManager::OnWriteResult,
                             this);
    if (rc != 0) {
      FailConnection("ble_gattc_write_flat", rc);
      return;
    }
  }
}

int BleManager::OnWriteResult(uint16_t conn_handle,
                              const struct ble_gatt_error* error,
                              struct ble_gatt_attr* /*attr*/, void* /*arg*/) {
  // Resolved via instance_, not arg -- see the comment in OnGapEvent().
  auto* self = instance_;
  if (self == nullptr || error == nullptr) {
    return 0;
  }
  if (conn_handle != self->conn_handle_) {
    // Stale result for a connection this code has already moved on from --
    // see the identical check in OnSvcDisc().
    ESP_LOGD(kTag, "ignoring stale write result for conn_handle=%d",
             conn_handle);
    return 0;
  }
  self->HandleWriteResult(*error);
  return 0;
}

void BleManager::HandleWriteResult(const struct ble_gatt_error& error) {
  if (error.status != 0) {
    FailConnection("write", error.status);
    return;
  }
  ESP_LOGI(kTag, "Write succeeded");
}

void BleManager::HandleNotifyRx(const struct ble_gap_event& event) {
  if (event.notify_rx.conn_handle != conn_handle_) {
    // Stale result for a connection this code has already moved on from --
    // see the identical check in OnSvcDisc().
    ESP_LOGD(kTag, "ignoring stale notification for conn_handle=%d",
             event.notify_rx.conn_handle);
    return;
  }

  BleCharacteristicEvent notify_event;
  notify_event.kind = BleCharacteristicEventKind::kNotification;
  notify_event.notify_handle = event.notify_rx.attr_handle;

  const uint16_t total_len = OS_MBUF_PKTLEN(event.notify_rx.om);
  const size_t copy_len =
      total_len < BleCharacteristicEvent::kMaxPayloadSize
          ? total_len
          : BleCharacteristicEvent::kMaxPayloadSize;
  os_mbuf_copydata(event.notify_rx.om, 0, static_cast<int>(copy_len),
                   notify_event.payload);
  notify_event.payload_len = copy_len;
  if (total_len > copy_len) {
    // Never actually reachable at this build's 256-byte MTU (max payload
    // 253 bytes, exactly kMaxPayloadSize), but if a future MTU renegotiation
    // ever changed that, this would be the only sign of it -- surfaced
    // rather than silently dropped.
    ESP_LOGW(kTag, "notification payload truncated: %u bytes, kept %u",
             total_len, static_cast<unsigned>(copy_len));
  }

  EnqueueCharacteristic(notify_event);
}

void BleManager::HandleDisconnect(const struct ble_gap_event& event) {
  if (event.disconnect.conn.conn_handle != conn_handle_) {
    // A disconnect for a connection this code has already moved on from
    // (superseded by a newer one, or already cleaned up) -- see the
    // identical check in OnSvcDisc(). Nothing to do: FailConnection() has
    // already run for whatever this stale handle belonged to.
    ESP_LOGD(kTag, "ignoring stale disconnect for conn_handle=%d",
             event.disconnect.conn.conn_handle);
    return;
  }
  FailConnection("disconnect", event.disconnect.reason);
}

void BleManager::FailConnection(const char* what, int reason) {
  ESP_LOGE(kTag, "%s failed/ended; reason=%d", what, reason);

  if (conn_handle_ != BLE_HS_CONN_HANDLE_NONE) {
    // Only reached for a discovery-level failure -- the connection is still
    // up and needs to be actively torn down, matching the reference NimBLE
    // client (blecent)'s own "discovery failed -> terminate" pattern. A
    // genuine BLE_GAP_EVENT_DISCONNECT means the link is already gone, so
    // this call would simply fail harmlessly (checked, not ignored).
    const int rc = ble_gap_terminate(conn_handle_, BLE_ERR_REM_USER_CONN_TERM);
    if (rc != 0 && rc != BLE_HS_ENOTCONN) {
      ESP_LOGW(kTag, "ble_gap_terminate failed: %d", rc);
    }
  }
  conn_handle_ = BLE_HS_CONN_HANDLE_NONE;
  num_services_ = 0;
  next_svc_to_disc_ = 0;
  pending_subscribe_val_handle_ = 0;
  descriptor_search_started_ = false;
  subscribe_started_ = false;

  // A connect-attempt failure -- whether it never left kDeviceFound (a
  // synchronous ble_hs_id_infer_auto()/ble_gap_connect() error inside
  // Connect(), before SetState(kConnecting) ever runs) or failed while
  // kConnecting (an async BLE_GAP_EVENT_CONNECT with a nonzero status) --
  // never produced a connection, so the appliance's address stays valid for
  // a future retry and the state machine returns to kDeviceFound rather than
  // kIdle. Anything past that point (kConnected or later) reflects a
  // connection that did exist and has now ended, hence kDisconnected -- the
  // state this enum has reserved for exactly that since M3.1.
  const bool never_connected =
      state_ == BleState::kDeviceFound || state_ == BleState::kConnecting;
  SetState(never_connected ? BleState::kDeviceFound
                           : BleState::kDisconnected);
  Enqueue(BleEvent{BleEventType::kConnectionFailed, reason});
}

void BleManager::OnHostReset(int reason) {
  ESP_LOGW(kTag, "NimBLE host reset; reason=%d", reason);
  if (instance_ == nullptr) {
    return;
  }
  instance_->SetState(BleState::kIdle);
  instance_->Enqueue(BleEvent{BleEventType::kHostReset, reason});
}

}  // namespace grohe_ble
