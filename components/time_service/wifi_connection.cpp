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

// Diagnostic-only: symbolic name for a WIFI_EVENT_STA_DISCONNECTED reason
// code (wifi_err_reason_t, esp_wifi_types_generic.h), so a disconnect log
// never has to be read as a bare number. Covers the full enum as of this
// ESP-IDF version; an unrecognized value (a future ESP-IDF adding new
// reasons, or a genuinely malformed one) falls through to "UNKNOWN" rather
// than guessing.
const char* WifiReasonToString(uint8_t reason) {
  switch (reason) {
    case WIFI_REASON_UNSPECIFIED: return "WIFI_REASON_UNSPECIFIED";
    case WIFI_REASON_AUTH_EXPIRE: return "WIFI_REASON_AUTH_EXPIRE";
    case WIFI_REASON_AUTH_LEAVE: return "WIFI_REASON_AUTH_LEAVE";
    case WIFI_REASON_ASSOC_EXPIRE: return "WIFI_REASON_ASSOC_EXPIRE";
    case WIFI_REASON_ASSOC_TOOMANY: return "WIFI_REASON_ASSOC_TOOMANY";
    case WIFI_REASON_NOT_AUTHED: return "WIFI_REASON_NOT_AUTHED";
    case WIFI_REASON_NOT_ASSOCED: return "WIFI_REASON_NOT_ASSOCED";
    case WIFI_REASON_ASSOC_LEAVE: return "WIFI_REASON_ASSOC_LEAVE";
    case WIFI_REASON_ASSOC_NOT_AUTHED: return "WIFI_REASON_ASSOC_NOT_AUTHED";
    case WIFI_REASON_DISASSOC_PWRCAP_BAD: return "WIFI_REASON_DISASSOC_PWRCAP_BAD";
    case WIFI_REASON_DISASSOC_SUPCHAN_BAD: return "WIFI_REASON_DISASSOC_SUPCHAN_BAD";
    case WIFI_REASON_BSS_TRANSITION_DISASSOC: return "WIFI_REASON_BSS_TRANSITION_DISASSOC";
    case WIFI_REASON_IE_INVALID: return "WIFI_REASON_IE_INVALID";
    case WIFI_REASON_MIC_FAILURE: return "WIFI_REASON_MIC_FAILURE";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT";
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT: return "WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT";
    case WIFI_REASON_IE_IN_4WAY_DIFFERS: return "WIFI_REASON_IE_IN_4WAY_DIFFERS";
    case WIFI_REASON_GROUP_CIPHER_INVALID: return "WIFI_REASON_GROUP_CIPHER_INVALID";
    case WIFI_REASON_PAIRWISE_CIPHER_INVALID: return "WIFI_REASON_PAIRWISE_CIPHER_INVALID";
    case WIFI_REASON_AKMP_INVALID: return "WIFI_REASON_AKMP_INVALID";
    case WIFI_REASON_UNSUPP_RSN_IE_VERSION: return "WIFI_REASON_UNSUPP_RSN_IE_VERSION";
    case WIFI_REASON_INVALID_RSN_IE_CAP: return "WIFI_REASON_INVALID_RSN_IE_CAP";
    case WIFI_REASON_802_1X_AUTH_FAILED: return "WIFI_REASON_802_1X_AUTH_FAILED";
    case WIFI_REASON_CIPHER_SUITE_REJECTED: return "WIFI_REASON_CIPHER_SUITE_REJECTED";
    case WIFI_REASON_TDLS_PEER_UNREACHABLE: return "WIFI_REASON_TDLS_PEER_UNREACHABLE";
    case WIFI_REASON_TDLS_UNSPECIFIED: return "WIFI_REASON_TDLS_UNSPECIFIED";
    case WIFI_REASON_SSP_REQUESTED_DISASSOC: return "WIFI_REASON_SSP_REQUESTED_DISASSOC";
    case WIFI_REASON_NO_SSP_ROAMING_AGREEMENT: return "WIFI_REASON_NO_SSP_ROAMING_AGREEMENT";
    case WIFI_REASON_BAD_CIPHER_OR_AKM: return "WIFI_REASON_BAD_CIPHER_OR_AKM";
    case WIFI_REASON_NOT_AUTHORIZED_THIS_LOCATION: return "WIFI_REASON_NOT_AUTHORIZED_THIS_LOCATION";
    case WIFI_REASON_SERVICE_CHANGE_PERCLUDES_TS: return "WIFI_REASON_SERVICE_CHANGE_PERCLUDES_TS";
    case WIFI_REASON_UNSPECIFIED_QOS: return "WIFI_REASON_UNSPECIFIED_QOS";
    case WIFI_REASON_NOT_ENOUGH_BANDWIDTH: return "WIFI_REASON_NOT_ENOUGH_BANDWIDTH";
    case WIFI_REASON_MISSING_ACKS: return "WIFI_REASON_MISSING_ACKS";
    case WIFI_REASON_EXCEEDED_TXOP: return "WIFI_REASON_EXCEEDED_TXOP";
    case WIFI_REASON_STA_LEAVING: return "WIFI_REASON_STA_LEAVING";
    case WIFI_REASON_END_BA: return "WIFI_REASON_END_BA";
    case WIFI_REASON_UNKNOWN_BA: return "WIFI_REASON_UNKNOWN_BA";
    case WIFI_REASON_TIMEOUT: return "WIFI_REASON_TIMEOUT";
    case WIFI_REASON_PEER_INITIATED: return "WIFI_REASON_PEER_INITIATED";
    case WIFI_REASON_AP_INITIATED: return "WIFI_REASON_AP_INITIATED";
    case WIFI_REASON_INVALID_FT_ACTION_FRAME_COUNT: return "WIFI_REASON_INVALID_FT_ACTION_FRAME_COUNT";
    case WIFI_REASON_INVALID_PMKID: return "WIFI_REASON_INVALID_PMKID";
    case WIFI_REASON_INVALID_MDE: return "WIFI_REASON_INVALID_MDE";
    case WIFI_REASON_INVALID_FTE: return "WIFI_REASON_INVALID_FTE";
    case WIFI_REASON_TRANSMISSION_LINK_ESTABLISH_FAILED: return "WIFI_REASON_TRANSMISSION_LINK_ESTABLISH_FAILED";
    case WIFI_REASON_ALTERATIVE_CHANNEL_OCCUPIED: return "WIFI_REASON_ALTERATIVE_CHANNEL_OCCUPIED";
    case WIFI_REASON_BEACON_TIMEOUT: return "WIFI_REASON_BEACON_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND: return "WIFI_REASON_NO_AP_FOUND";
    case WIFI_REASON_AUTH_FAIL: return "WIFI_REASON_AUTH_FAIL";
    case WIFI_REASON_ASSOC_FAIL: return "WIFI_REASON_ASSOC_FAIL";
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return "WIFI_REASON_HANDSHAKE_TIMEOUT";
    case WIFI_REASON_CONNECTION_FAIL: return "WIFI_REASON_CONNECTION_FAIL";
    case WIFI_REASON_AP_TSF_RESET: return "WIFI_REASON_AP_TSF_RESET";
    case WIFI_REASON_ROAMING: return "WIFI_REASON_ROAMING";
    case WIFI_REASON_ASSOC_COMEBACK_TIME_TOO_LONG: return "WIFI_REASON_ASSOC_COMEBACK_TIME_TOO_LONG";
    case WIFI_REASON_SA_QUERY_TIMEOUT: return "WIFI_REASON_SA_QUERY_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY: return "WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD: return "WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD: return "WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD";
    default: return "UNKNOWN";
  }
}

// Diagnostic-only: symbolic name for the auth mode WIFI_EVENT_STA_CONNECTED
// reports (wifi_auth_mode_t) -- same "no numeric-only logs" reasoning as
// WifiReasonToString() above.
const char* WifiAuthModeToString(wifi_auth_mode_t mode) {
  switch (mode) {
    case WIFI_AUTH_OPEN: return "WIFI_AUTH_OPEN";
    case WIFI_AUTH_WEP: return "WIFI_AUTH_WEP";
    case WIFI_AUTH_WPA_PSK: return "WIFI_AUTH_WPA_PSK";
    case WIFI_AUTH_WPA2_PSK: return "WIFI_AUTH_WPA2_PSK";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WIFI_AUTH_WPA_WPA2_PSK";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WIFI_AUTH_WPA2_ENTERPRISE";
    case WIFI_AUTH_WPA3_PSK: return "WIFI_AUTH_WPA3_PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WIFI_AUTH_WPA2_WPA3_PSK";
    case WIFI_AUTH_WAPI_PSK: return "WIFI_AUTH_WAPI_PSK";
    case WIFI_AUTH_OWE: return "WIFI_AUTH_OWE";
    case WIFI_AUTH_WPA3_ENT_192: return "WIFI_AUTH_WPA3_ENT_192";
    case WIFI_AUTH_WPA3_EXT_PSK: return "WIFI_AUTH_WPA3_EXT_PSK";
    case WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE: return "WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE";
    case WIFI_AUTH_DPP: return "WIFI_AUTH_DPP";
    default: return "UNKNOWN";
  }
}
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
  ESP_LOGI(kTag,
           "AcquireAsync: prev_ref_count=%d new_ref_count=%d "
           "will_start_connecting=%d bits=0x%02x",
           prev, prev + 1, prev == 0,
           static_cast<unsigned>(xEventGroupGetBits(events_)));
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
  ESP_LOGI(kTag,
           "Acquire: prev_ref_count=%d new_ref_count=%d "
           "will_start_connecting=%d bits=0x%02x",
           prev, prev + 1, prev == 0,
           static_cast<unsigned>(xEventGroupGetBits(events_)));
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
  const bool connected = (bits & kConnectedBit) != 0;
  ESP_LOGI(kTag, "Acquire: result=%d bits=0x%02x", connected,
           static_cast<unsigned>(bits));
  return connected;
}

void WifiConnection::Release() {
  const int prev = ref_count_.fetch_sub(1);
  ESP_LOGI(kTag,
           "Release: prev_ref_count=%d new_ref_count=%d "
           "will_tear_down=%d bits=0x%02x",
           prev, prev - 1, prev == 1,
           static_cast<unsigned>(xEventGroupGetBits(events_)));
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
                                         void* data) {
  // Logged unconditionally, before any filtering below, so every event
  // this handler is ever invoked for is visible -- including ones the
  // guards below go on to drop as stale/late, which is itself the
  // diagnostic signal for an event-ordering problem (see the header's own
  // comment on why a stale post-teardown event can arrive here at all).
  ESP_LOGI(kTag, "event received: base=%s id=%d",
           base == WIFI_EVENT ? "WIFI_EVENT"
           : base == IP_EVENT ? "IP_EVENT"
                              : "?",
           static_cast<int>(id));

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
    ESP_LOGI(kTag, "event dropped: ref_count=0 (stale, no active cycle)");
    return;
  }
  const EventBits_t bits_on_entry = xEventGroupGetBits(events_);
  if (bits_on_entry & (kConnectedBit | kFailedBit)) {
    ESP_LOGI(kTag, "event dropped: cycle already resolved, bits=0x%02x",
             static_cast<unsigned>(bits_on_entry));
    return;  // Already resolved this cycle; ignore a late/stray event.
  }
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    ESP_LOGI(kTag, "WIFI_EVENT_STA_START");
    TryConnect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
    // L2 association only -- not yet a usable IP connection. Tracked
    // separately (not an event-group bit -- see the header's own comment)
    // purely so IP_EVENT_STA_GOT_IP below has something real to require
    // before it's allowed to resolve kConnectedBit. Never resolves
    // Acquire()/AcquireAsync() by itself.
    const auto* connected_data =
        static_cast<const wifi_event_sta_connected_t*>(data);
    ESP_LOGI(kTag,
             "Wi-Fi associated; waiting for IP: bssid=%02x:%02x:%02x:%02x:%02x:%02x "
             "channel=%d authmode=%d (%s)",
             connected_data->bssid[0], connected_data->bssid[1],
             connected_data->bssid[2], connected_data->bssid[3],
             connected_data->bssid[4], connected_data->bssid[5],
             connected_data->channel, connected_data->authmode,
             WifiAuthModeToString(connected_data->authmode));
    sta_connected_ = true;
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    const auto* disconnected_data =
        static_cast<const wifi_event_sta_disconnected_t*>(data);
    sta_connected_ = false;
    if (retry_count_ < kMaxWifiRetries) {
      ++retry_count_;
      ESP_LOGW(kTag,
               "Wi-Fi disconnected: reason=%d (%s) retry=%d/%d ref_count=%d",
               disconnected_data->reason,
               WifiReasonToString(disconnected_data->reason), retry_count_,
               kMaxWifiRetries, ref_count_.load());
      TryConnect();
    } else {
      ESP_LOGE(kTag,
               "Wi-Fi connect failed after %d retries; giving up: "
               "reason=%d (%s) ref_count=%d",
               kMaxWifiRetries, disconnected_data->reason,
               WifiReasonToString(disconnected_data->reason),
               ref_count_.load());
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
    const auto* got_ip_data = static_cast<const ip_event_got_ip_t*>(data);
    ESP_LOGI(kTag,
             "Wi-Fi connected (associated + IP acquired): ip=" IPSTR
             " gw=" IPSTR " netmask=" IPSTR,
             IP2STR(&got_ip_data->ip_info.ip),
             IP2STR(&got_ip_data->ip_info.gw),
             IP2STR(&got_ip_data->ip_info.netmask));
    Resolve(true);
  }
}

void WifiConnection::Resolve(bool connected) {
  ESP_LOGI(kTag, "Resolve: before: success=%d bits=0x%02x", connected,
           static_cast<unsigned>(xEventGroupGetBits(events_)));
  xEventGroupSetBits(events_, connected ? kConnectedBit : kFailedBit);
  ESP_LOGI(kTag, "Resolve: after: bits=0x%02x",
           static_cast<unsigned>(xEventGroupGetBits(events_)));
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
  ESP_LOGI(kTag, "TearDownWifi: ref_count=%d bits=0x%02x", ref_count_.load(),
           static_cast<unsigned>(xEventGroupGetBits(events_)));
  esp_wifi_stop();
  esp_wifi_deinit();
  if (sta_netif_ != nullptr) {
    esp_netif_destroy(sta_netif_);
    sta_netif_ = nullptr;
  }
  ESP_LOGI(kTag, "Wi-Fi torn down");
}

}  // namespace time_service
