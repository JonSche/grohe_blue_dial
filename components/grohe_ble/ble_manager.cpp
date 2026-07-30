#include "grohe_ble/ble_manager.hpp"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "grohe_ble/ble_constants.hpp"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/hci_common.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "services/gap/ble_svc_gap.h"

namespace grohe_ble {
namespace {
constexpr char kTag[] = "ble_manager";
constexpr char kDeviceName[] = "grohe-dial";
// Events are rare (a handful of state changes, not a data stream), so a
// small bounded depth is generous, not tight.
constexpr UBaseType_t kEventQueueDepth = 8;

// Scan interval and window are set to this same value, giving a continuous
// (100% duty cycle) scan -- see StartScan().
constexpr uint32_t kScanIntervalMs = 30;

// "aa:bb:cc:dd:ee:ff" + NUL.
constexpr size_t kAddrStrSize = 18;
// Legacy advertising payloads are at most 31 bytes, so these bounds are
// never actually reached; they exist so the formatters below can't overrun
// regardless of what a peer sends.
constexpr size_t kNameBufSize = 32;
constexpr size_t kHexBufSize = 2 * 31 + 1;
constexpr size_t kUuidListBufSize = 4 * BLE_UUID_STR_LEN;

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

  instance_ = this;
  SetState(BleState::kInitializing);

  err = nimble_port_init();
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "nimble_port_init failed: %s", esp_err_to_name(err));
    return err;
  }

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

int BleManager::OnGapEvent(struct ble_gap_event* event, void* arg) {
  auto* self = static_cast<BleManager*>(arg);
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
