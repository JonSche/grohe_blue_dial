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
    retry_count_ = 0;
    sta_connected_ = false;
    // Clearing here, not in Release(), matters: see the top-of-file
    // comment for why this is what actually starts a fresh cycle clean,
    // and why a stale event from the *previous* cycle's teardown is
    // instead handled by HandleWifiOrIpEvent()'s own ref_count_ guard,
    // not by clear-timing.
    xEventGroupClearBits(events_, kConnectedBit | kFailedBit);
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
  // caller to actually reach StartConnecting()/xEventGroupClearBits() --
  // in theory it could then observe a bit left over from *two* cycles
  // back. This codebase's actual callers (SntpTimeProvider::Init(),
  // OtaManager's CheckForUpdate()/StartUpdate()) only ever run on the
  // app task, one at a time, so this isn't reachable today -- documented
  // for whichever caller relies on it next, not fixed, since there's no
  // real caller to fix it for.
  const int prev = ref_count_.fetch_add(1);
  if (prev == 0) {
    retry_count_ = 0;
    sta_connected_ = false;
    xEventGroupClearBits(events_, kConnectedBit | kFailedBit);
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
  if (ref_count_.load() == 0) {
    // No acquirer holds a connection right now -- this can only be a
    // stale event generated by our own teardown (esp_wifi_stop(), called
    // from TearDownWifi() once the last Release() ran, posts
    // WIFI_EVENT_STA_DISCONNECTED asynchronously if the STA was still
    // associated, same as any other disconnect -- see the header's own
    // top comment). There is no live cycle for it to belong to and
    // nothing to resolve or retry: drop it before it can touch
    // retry_count_, sta_connected_, or either event-group bit. This is
    // the actual fix for a stale post-teardown event poisoning the next
    // cycle -- not the bits, which are cleared at the *start* of the
    // next Acquire()/AcquireAsync() instead (see those functions).
    return;
  }
  if (xEventGroupGetBits(events_) & (kConnectedBit | kFailedBit)) {
    return;  // Already resolved this cycle; ignore a late/stray event.
  }
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    TryConnect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
    // L2 association only -- not yet a usable IP connection. Tracked
    // separately (not an event-group bit -- see the header's own comment)
    // purely so IP_EVENT_STA_GOT_IP below has something real to require
    // before it's allowed to resolve kConnectedBit. Never resolves
    // Acquire()/AcquireAsync() by itself.
    ESP_LOGI(kTag, "Wi-Fi associated; waiting for IP");
    sta_connected_ = true;
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    sta_connected_ = false;
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
    // ESP-IDF's own contract guarantees STA_CONNECTED always precedes
    // GOT_IP (DHCP only ever starts after L2 association), so this
    // should never actually fire -- kept as a hard requirement, not just
    // a comment, per the explicit "STA_CONNECTED AND GOT_IP" contract
    // Acquire()/AcquireAsync() callers rely on: OtaManager in particular
    // starts HTTPS the instant Acquire() returns success, with no Wi-Fi
    // check of its own, so this class -- not its caller -- is the one
    // place that must actually enforce it.
    if (!sta_connected_) {
      ESP_LOGE(kTag, "GOT_IP without a prior STA_CONNECTED; ignoring");
      return;
    }
    ESP_LOGI(kTag, "Wi-Fi connected (associated + IP acquired)");
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
  // esp_wifi_stop() posts WIFI_EVENT_STA_DISCONNECTED asynchronously if
  // the STA was still associated, exactly like a real disconnect --
  // ref_count_ is already 0 by the time Release() calls this (see its
  // own comment), so HandleWifiOrIpEvent()'s ref_count_ guard is what
  // keeps that self-inflicted event from being mistaken for a real one
  // whenever the event loop task eventually gets around to it.
  esp_wifi_stop();
  esp_wifi_deinit();
  if (sta_netif_ != nullptr) {
    esp_netif_destroy(sta_netif_);
    sta_netif_ = nullptr;
  }
  ESP_LOGI(kTag, "Wi-Fi torn down");
}

}  // namespace time_service
