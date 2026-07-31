#include "time_service/sntp_time_provider.hpp"

#include <cstdio>
#include <cstring>

#include "apps/esp_sntp.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "nvs_flash.h"

// This provider connects to Wi-Fi purely as a one-shot SNTP time source,
// then fully tears Wi-Fi/lwIP back down -- never a runtime dependency for
// anything else in this firmware (see the header's own comment). The
// design is entirely event-driven, with no dedicated FreeRTOS task and no
// blocking wait, mirroring how grohe_ble::BleManager itself never spins up
// a task to poll NimBLE -- it reacts to NimBLE's own callbacks. Here:
//
//   WIFI_EVENT_STA_START/DISCONNECTED, IP_EVENT_STA_GOT_IP  -- Wi-Fi's own
//   async events, handled by HandleWifiOrIpEvent() on esp_event's default
//   loop task.
//
//   SNTP's own sync-notification callback (OnTimeSyncNotification) and the
//   SNTP timeout esp_timer (OnSntpTimeoutTimer) do NOT run on that same
//   task -- the former fires on lwIP's own tcpip task, the latter on
//   esp_timer's own service task. Rather than adding real synchronization
//   (a mutex) around the shared retry/teardown state, both immediately
//   re-post as a custom event (kInternalEventBase) onto the *same* default
//   event loop WIFI_EVENT/IP_EVENT already use. esp_event guarantees every
//   handler on one loop runs serialized on that loop's single task, so
//   OnInternalEvent() ends up handling synced/timeout exactly like a third
//   kind of Wi-Fi event -- meaning every piece of mutable state this class
//   has (other than the one atomic `valid_`, read from a different task
//   entirely) is genuinely touched from just one task, without needing its
//   own lock.
namespace time_service {
namespace {
constexpr char kTag[] = "sntp_time_provider";
constexpr char kSntpServer[] = "pool.ntp.org";
constexpr int kMaxWifiRetries = 5;
constexpr uint64_t kSntpTimeoutUs = 15'000'000;  // 15 s

ESP_EVENT_DEFINE_BASE(kInternalEventBase);
enum InternalEventId {
  kInternalEventSynced,
  kInternalEventTimeout,
};

// Foreign-task callbacks (lwIP's tcpip task, esp_timer's service task) post
// with a short, bounded timeout rather than blocking indefinitely -- if the
// default event loop's own queue were ever full, dropping this one post is
// preferable to stalling someone else's task.
constexpr TickType_t kPostTimeout = pdMS_TO_TICKS(1000);
}  // namespace

SntpTimeProvider* SntpTimeProvider::instance_ = nullptr;

SntpTimeProvider::SntpTimeProvider(const WifiCredentialsProvider& wifi_credentials)
    : wifi_credentials_(wifi_credentials) {
  instance_ = this;
}

SntpTimeProvider::~SntpTimeProvider() {
  if (instance_ == this) {
    instance_ = nullptr;
  }
}

esp_err_t SntpTimeProvider::Init() {
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

  err = esp_netif_init();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(kTag, "esp_netif_init failed: %s", esp_err_to_name(err));
    return err;
  }

  err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(kTag, "esp_event_loop_create_default failed: %s",
             esp_err_to_name(err));
    return err;
  }

  sta_netif_ = esp_netif_create_default_wifi_sta();
  if (sta_netif_ == nullptr) {
    ESP_LOGE(kTag, "esp_netif_create_default_wifi_sta failed");
    return ESP_FAIL;
  }

  wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
  err = esp_wifi_init(&wifi_init_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "esp_wifi_init failed: %s", esp_err_to_name(err));
    return err;
  }

  err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            &OnWifiOrIpEvent, nullptr, nullptr);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "register WIFI_EVENT handler failed: %s",
             esp_err_to_name(err));
    return err;
  }
  err = esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &OnWifiOrIpEvent, nullptr, nullptr);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "register IP_EVENT handler failed: %s",
             esp_err_to_name(err));
    return err;
  }
  err = esp_event_handler_instance_register(kInternalEventBase,
                                            ESP_EVENT_ANY_ID, &OnInternalEvent,
                                            nullptr, nullptr);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "register internal event handler failed: %s",
             esp_err_to_name(err));
    return err;
  }

  const WifiCredentials& creds = wifi_credentials_.Get();
  wifi_config_t wifi_config = {};
  std::snprintf(reinterpret_cast<char*>(wifi_config.sta.ssid),
               sizeof(wifi_config.sta.ssid), "%s", creds.ssid);
  std::snprintf(reinterpret_cast<char*>(wifi_config.sta.password),
               sizeof(wifi_config.sta.password), "%s", creds.password);

  err = esp_wifi_set_mode(WIFI_MODE_STA);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
    return err;
  }
  err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
    return err;
  }
  err = esp_wifi_start();
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "esp_wifi_start failed: %s", esp_err_to_name(err));
    return err;
  }

  ESP_LOGI(kTag, "Wi-Fi/SNTP time sync started (event-driven, non-blocking)");
  return ESP_OK;
}

bool SntpTimeProvider::GetCurrentEpoch(uint32_t* out_epoch_seconds) const {
  if (!valid_) {
    return false;
  }
  const time_t now = time(nullptr);
  if (now < 0) {
    return false;
  }
  *out_epoch_seconds = static_cast<uint32_t>(now);
  return true;
}

void SntpTimeProvider::OnWifiOrIpEvent(void* /*arg*/, esp_event_base_t base,
                                       int32_t id, void* data) {
  // Resolved via instance_, not arg -- see the header's own comment,
  // matching grohe_ble::BleManager's established discipline.
  auto* self = instance_;
  if (self == nullptr) {
    return;
  }
  self->HandleWifiOrIpEvent(base, id, data);
}

void SntpTimeProvider::TryConnect() {
  // esp_wifi_connect() can fail *synchronously* (e.g. ESP_ERR_WIFI_SSID for
  // an empty/invalid SSID -- confirmed on hardware: with the placeholder
  // empty credentials, this returned an error and never fired
  // WIFI_EVENT_STA_DISCONNECTED, meaning nothing would have torn Wi-Fi/
  // lwIP down at all if this return value went unchecked. Retrying an
  // input that's synchronously rejected would just fail identically, so
  // this is treated as an immediate give-up, not counted against
  // kMaxWifiRetries (which is for *connection* failures after a scan/
  // associate attempt genuinely started).
  const esp_err_t err = esp_wifi_connect();
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "esp_wifi_connect failed: %s; giving up",
             esp_err_to_name(err));
    TearDown();
  }
}

void SntpTimeProvider::HandleWifiOrIpEvent(esp_event_base_t base, int32_t id,
                                           void* /*data*/) {
  if (torn_down_) {
    return;  // Already succeeded or given up; ignore anything further.
  }
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    TryConnect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    if (retry_count_ < kMaxWifiRetries) {
      ++retry_count_;
      ESP_LOGW(kTag, "Wi-Fi disconnected, retry %d/%d", retry_count_,
               kMaxWifiRetries);
      TryConnect();
    } else {
      ESP_LOGE(kTag, "Wi-Fi connect failed after %d retries; giving up",
               kMaxWifiRetries);
      TearDown();
    }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    ESP_LOGI(kTag, "Wi-Fi connected; starting SNTP sync");
    StartSntp();
  }
}

void SntpTimeProvider::StartSntp() {
  sntp_set_time_sync_notification_cb(&OnTimeSyncNotification);
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, kSntpServer);
  esp_sntp_init();

  const esp_timer_create_args_t timer_args = {
      .callback = &OnSntpTimeoutTimer,
      .arg = nullptr,
      .name = "sntp_timeout",
  };
  const esp_err_t err = esp_timer_create(&timer_args, &sntp_timeout_timer_);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "esp_timer_create failed: %s", esp_err_to_name(err));
    TearDown();
    return;
  }
  esp_timer_start_once(sntp_timeout_timer_, kSntpTimeoutUs);
}

void SntpTimeProvider::OnTimeSyncNotification(struct timeval* /*tv*/) {
  // Runs on lwIP's own tcpip task, not the default event loop's task --
  // re-post rather than touch any shared state directly here. See the
  // top-of-file comment for why.
  esp_event_post(kInternalEventBase, kInternalEventSynced, nullptr, 0,
                 kPostTimeout);
}

void SntpTimeProvider::OnSntpTimeoutTimer(void* /*arg*/) {
  // Runs on esp_timer's own service task -- same reasoning as
  // OnTimeSyncNotification() above.
  esp_event_post(kInternalEventBase, kInternalEventTimeout, nullptr, 0,
                 kPostTimeout);
}

void SntpTimeProvider::OnInternalEvent(void* /*arg*/, esp_event_base_t /*base*/,
                                       int32_t id, void* /*data*/) {
  auto* self = instance_;
  if (self == nullptr) {
    return;
  }
  if (id == kInternalEventSynced) {
    self->HandleSynced();
  } else if (id == kInternalEventTimeout) {
    self->HandleSntpTimeout();
  }
}

void SntpTimeProvider::HandleSynced() {
  if (torn_down_) {
    return;  // The timeout already fired and gave up first; ignore.
  }
  ESP_LOGI(kTag, "SNTP sync succeeded; system clock set");
  valid_ = true;
  TearDown();
}

void SntpTimeProvider::HandleSntpTimeout() {
  if (torn_down_) {
    return;  // Sync already succeeded first; ignore.
  }
  ESP_LOGE(kTag, "SNTP sync timed out; giving up");
  TearDown();
}

void SntpTimeProvider::TearDown() {
  if (torn_down_) {
    return;
  }
  torn_down_ = true;

  if (sntp_timeout_timer_ != nullptr) {
    esp_timer_stop(sntp_timeout_timer_);
    esp_timer_delete(sntp_timeout_timer_);
    sntp_timeout_timer_ = nullptr;
  }
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }
  esp_wifi_stop();
  esp_wifi_deinit();
  if (sta_netif_ != nullptr) {
    esp_netif_destroy(sta_netif_);
    sta_netif_ = nullptr;
  }
  ESP_LOGI(kTag, "Wi-Fi/SNTP torn down; %s", valid_ ? "time available"
                                                    : "time NOT available");
}

}  // namespace time_service
