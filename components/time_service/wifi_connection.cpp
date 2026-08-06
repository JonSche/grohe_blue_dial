#include "time_service/wifi_connection.hpp"

#include <cstdio>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "nvs_flash.h"

// See the header's own top comment for the overall design. The connect-
// with-retry state machine below (TryConnect()/HandleWifiOrIpEvent()) is
// unchanged from the SntpTimeProvider this was extracted from -- only what
// happens at the two terminal points (success, give-up) changed: instead
// of directly starting SNTP or tearing everything down, this class sets an
// event-group bit and (if anyone's waiting on the non-blocking form)
// invokes a callback. Actual teardown is now exclusively Release()-driven
// (reference-counted), not triggered internally by a failure -- whoever
// called Acquire()/AcquireAsync() still owns exactly one Release() call
// regardless of the outcome, so nothing leaks; it just means a failure no
// longer tears down "for" the caller, the same way an exception doesn't
// free memory for you in a language without RAII.
namespace time_service {
namespace {
constexpr char kTag[] = "wifi_connection";
constexpr int kMaxWifiRetries = 5;

constexpr EventBits_t kConnectedBit = BIT0;
constexpr EventBits_t kFailedBit = BIT1;
}  // namespace

WifiConnection* WifiConnection::instance_ = nullptr;

WifiConnection::WifiConnection(const WifiCredentialsProvider& wifi_credentials)
    : wifi_credentials_(wifi_credentials) {
  instance_ = this;
}

WifiConnection::~WifiConnection() {
  if (instance_ == this) {
    instance_ = nullptr;
  }
}

esp_err_t WifiConnection::Init() {
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

  events_ = xEventGroupCreate();
  if (events_ == nullptr) {
    ESP_LOGE(kTag, "xEventGroupCreate failed");
    return ESP_ERR_NO_MEM;
  }

  return ESP_OK;
}

esp_err_t WifiConnection::StartConnecting() {
  sta_netif_ = esp_netif_create_default_wifi_sta();
  if (sta_netif_ == nullptr) {
    ESP_LOGE(kTag, "esp_netif_create_default_wifi_sta failed");
    xEventGroupSetBits(events_, kFailedBit);
    return ESP_FAIL;
  }

  wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_err_t err = esp_wifi_init(&wifi_init_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "esp_wifi_init failed: %s", esp_err_to_name(err));
    xEventGroupSetBits(events_, kFailedBit);
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
    xEventGroupSetBits(events_, kFailedBit);
    return err;
  }
  err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
    xEventGroupSetBits(events_, kFailedBit);
    return err;
  }
  err = esp_wifi_start();
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "esp_wifi_start failed: %s", esp_err_to_name(err));
    xEventGroupSetBits(events_, kFailedBit);
    return err;
  }

  ESP_LOGI(kTag, "Wi-Fi connecting...");
  return ESP_OK;
}

void WifiConnection::AcquireAsync(std::function<void()> on_ready,
                                  std::function<void()> on_failed) {
  const int prev = ref_count_.fetch_add(1);
  if (prev == 0) {
    // Bits are guaranteed already clear here -- Release() clears them
    // itself on the previous cycle's ->0 transition (see its own comment)
    // specifically so this 0->1 transition never has to race a concurrent
    // second caller (see Acquire()'s comment for the race this avoids).
    retry_count_ = 0;
    if (StartConnecting() != ESP_OK) {
      // Synchronous setup failure, on this calling task -- resolve
      // immediately, no async event is ever coming for this cycle.
      if (on_failed) {
        on_failed();
      }
      return;
    }
    pending_ready_ = std::move(on_ready);
    pending_failed_ = std::move(on_failed);
    return;
  }

  const EventBits_t bits = xEventGroupGetBits(events_);
  if (bits & kConnectedBit) {
    if (on_ready) {
      on_ready();
    }
  } else if (bits & kFailedBit) {
    if (on_failed) {
      on_failed();
    }
  }
  // Still connecting: see the header's own comment -- not reachable
  // today (this non-blocking form has exactly one caller, which only
  // ever calls it once).
}

bool WifiConnection::Acquire(TickType_t timeout) {
  // Note on the fetch_add/StartConnecting() ordering below: if a second
  // caller's own Acquire()/AcquireAsync() call landed concurrently on the
  // same 0->1 transition (fetch_add returning 1, "not first"), it goes
  // straight to xEventGroupWaitBits() below without waiting for this
  // caller to actually reach StartConnecting() -- safe only because the
  // bits it might observe are guaranteed already clear (from the *previous*
  // cycle's Release(), not this one), so it correctly blocks for this
  // fresh cycle's result rather than ever observing a stale one. This
  // codebase's actual callers (SntpTimeProvider::Init(), OtaManager's
  // CheckForUpdate()/StartUpdate()) only ever run on the app task, one at a
  // time, so this case isn't reachable today -- documented for whichever
  // caller relies on it next.
  const int prev = ref_count_.fetch_add(1);
  if (prev == 0) {
    retry_count_ = 0;
    if (StartConnecting() != ESP_OK) {
      return false;  // kFailedBit already set by StartConnecting() itself.
    }
  }
  const EventBits_t bits = xEventGroupWaitBits(
      events_, kConnectedBit | kFailedBit, pdFALSE, pdFALSE, timeout);
  return (bits & kConnectedBit) != 0;
}

void WifiConnection::Release() {
  const int prev = ref_count_.fetch_sub(1);
  if (prev == 1) {
    TearDownWifi();
    // Cleared here, not at the start of the next Acquire()/AcquireAsync()
    // 0->1 transition, so a fresh cycle never has a window where a second,
    // concurrent caller could observe the *previous* cycle's leftover bit
    // (see Acquire()'s own comment) -- by the time ref_count_ can reach 0
    // again, the bits are already guaranteed clear.
    xEventGroupClearBits(events_, kConnectedBit | kFailedBit);
  }
}

bool WifiConnection::IsConnected() const {
  return (xEventGroupGetBits(events_) & kConnectedBit) != 0;
}

void WifiConnection::OnWifiOrIpEvent(void* /*arg*/, esp_event_base_t base,
                                     int32_t id, void* data) {
  // Resolved via instance_, not arg -- matching this codebase's
  // established discipline (see grohe_ble::BleManager).
  auto* self = instance_;
  if (self == nullptr) {
    return;
  }
  self->HandleWifiOrIpEvent(base, id, data);
}

void WifiConnection::TryConnect() {
  // esp_wifi_connect() can fail *synchronously* (e.g. ESP_ERR_WIFI_SSID
  // for an empty/invalid SSID) without ever firing
  // WIFI_EVENT_STA_DISCONNECTED -- treated as an immediate give-up, not
  // counted against kMaxWifiRetries (which is for *connection* failures
  // after a scan/associate attempt genuinely started).
  const esp_err_t err = esp_wifi_connect();
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "esp_wifi_connect failed: %s; giving up",
             esp_err_to_name(err));
    Resolve(false);
  }
}

void WifiConnection::HandleWifiOrIpEvent(esp_event_base_t base, int32_t id,
                                         void* /*data*/) {
  if (xEventGroupGetBits(events_) & (kConnectedBit | kFailedBit)) {
    return;  // Already resolved this cycle; ignore a late/stray event.
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
      Resolve(false);
    }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    ESP_LOGI(kTag, "Wi-Fi connected");
    Resolve(true);
  }
}

void WifiConnection::Resolve(bool connected) {
  xEventGroupSetBits(events_, connected ? kConnectedBit : kFailedBit);
  std::function<void()> callback =
      connected ? std::move(pending_ready_) : std::move(pending_failed_);
  pending_ready_ = nullptr;
  pending_failed_ = nullptr;
  if (callback) {
    callback();
  }
}

void WifiConnection::TearDownWifi() {
  esp_wifi_stop();
  esp_wifi_deinit();
  if (sta_netif_ != nullptr) {
    esp_netif_destroy(sta_netif_);
    sta_netif_ = nullptr;
  }
  ESP_LOGI(kTag, "Wi-Fi torn down");
}

}  // namespace time_service
