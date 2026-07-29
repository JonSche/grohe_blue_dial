#include "grohe_ble/ble_manager.hpp"

#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
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
  instance_->SetState(BleState::kScanning);
  ESP_LOGI(kTag, "Scanning: not implemented yet");
  instance_->Enqueue(BleEvent{BleEventType::kHostSynced});
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
