#include "time_service/wifi_connection.hpp"

#include <cstdio>
#include <cstring>

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

// Diagnostic-only: symbolic name for wifi_mode_t.
const char* WifiModeToString(wifi_mode_t mode) {
  switch (mode) {
    case WIFI_MODE_NULL: return "WIFI_MODE_NULL";
    case WIFI_MODE_STA: return "WIFI_MODE_STA";
    case WIFI_MODE_AP: return "WIFI_MODE_AP";
    case WIFI_MODE_APSTA: return "WIFI_MODE_APSTA";
    case WIFI_MODE_NAN: return "WIFI_MODE_NAN";
    default: return "UNKNOWN";
  }
}

// Diagnostic-only: symbolic name for wifi_scan_method_t, logged alongside
// the STA config this class builds (see StartConnecting()).
const char* WifiScanMethodToString(wifi_scan_method_t method) {
  switch (method) {
    case WIFI_FAST_SCAN: return "WIFI_FAST_SCAN";
    case WIFI_ALL_CHANNEL_SCAN: return "WIFI_ALL_CHANNEL_SCAN";
    default: return "UNKNOWN";
  }
}

// Diagnostic-only: symbolic name for wifi_bandwidth_t.
const char* WifiBandwidthToString(wifi_bandwidth_t bw) {
  switch (bw) {
    case WIFI_BW_HT20: return "WIFI_BW_HT20 (20MHz)";
    case WIFI_BW_HT40: return "WIFI_BW_HT40 (40MHz)";
    case WIFI_BW80: return "WIFI_BW80 (80MHz)";
    case WIFI_BW160: return "WIFI_BW160 (160MHz)";
    case WIFI_BW80_BW80: return "WIFI_BW80_BW80 (80+80MHz)";
    default: return "UNKNOWN";
  }
}

// Diagnostic-only: WIFI_PROTOCOL_* bits (esp_wifi_types_generic.h) decoded
// into their standard names, space-separated -- "protocol bitmap" is
// otherwise just an opaque number.
void FormatWifiProtocolBitmap(uint8_t bitmap, char* out, size_t out_size) {
  struct Flag {
    uint8_t bit;
    const char* name;
  };
  static constexpr Flag kFlags[] = {
      {WIFI_PROTOCOL_11B, "11B"},   {WIFI_PROTOCOL_11G, "11G"},
      {WIFI_PROTOCOL_11N, "11N"},   {WIFI_PROTOCOL_LR, "LR"},
      {WIFI_PROTOCOL_11A, "11A"},   {WIFI_PROTOCOL_11AC, "11AC"},
      {WIFI_PROTOCOL_11AX, "11AX"},
  };
  size_t pos = 0;
  for (const auto& flag : kFlags) {
    if ((bitmap & flag.bit) == 0) {
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

// [driver]: read straight from the ESP-IDF driver right after
// esp_wifi_init() succeeds (see StartConnecting()) -- state that exists
// independent of anything this class configures itself, useful to confirm
// the driver actually came up in the state expected before this class
// hands it any of its own configuration. Every query is individually
// error-checked and logged as unavailable rather than skipped/asserted,
// since some (bandwidth, protocol) are only meaningful once the interface
// mode is set, which happens later in StartConnecting().
void LogDriverStateAfterInit(esp_netif_t* sta_netif) {
  uint8_t mac[6] = {};
  esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, mac);
  if (err == ESP_OK) {
    ESP_LOGI(kTag, "[driver] MAC: %02x:%02x:%02x:%02x:%02x:%02x", mac[0],
             mac[1], mac[2], mac[3], mac[4], mac[5]);
  } else {
    ESP_LOGW(kTag, "[driver] esp_wifi_get_mac failed: %s",
             esp_err_to_name(err));
  }

  wifi_country_t country = {};
  err = esp_wifi_get_country(&country);
  if (err == ESP_OK) {
    ESP_LOGI(kTag,
             "[driver] country: cc=%.3s start_channel=%u num_channels=%u "
             "max_tx_power=%d policy=%d",
             country.cc, country.schan, country.nchan, country.max_tx_power,
             country.policy);
  } else {
    ESP_LOGW(kTag, "[driver] esp_wifi_get_country failed: %s",
             esp_err_to_name(err));
  }

  const char* hostname = nullptr;
  err = esp_netif_get_hostname(sta_netif, &hostname);
  if (err == ESP_OK) {
    ESP_LOGI(kTag, "[driver] hostname: %s", hostname != nullptr ? hostname : "(null)");
  } else {
    ESP_LOGW(kTag, "[driver] esp_netif_get_hostname failed: %s",
             esp_err_to_name(err));
  }

  wifi_bandwidth_t bw;
  err = esp_wifi_get_bandwidth(WIFI_IF_STA, &bw);
  if (err == ESP_OK) {
    ESP_LOGI(kTag, "[driver] bandwidth: %s", WifiBandwidthToString(bw));
  } else {
    ESP_LOGW(kTag,
             "[driver] esp_wifi_get_bandwidth failed: %s (expected before "
             "esp_wifi_set_mode())",
             esp_err_to_name(err));
  }

  uint8_t protocol_bitmap = 0;
  err = esp_wifi_get_protocol(WIFI_IF_STA, &protocol_bitmap);
  if (err == ESP_OK) {
    char protocol_str[48];
    FormatWifiProtocolBitmap(protocol_bitmap, protocol_str, sizeof(protocol_str));
    ESP_LOGI(kTag, "[driver] protocol: 0x%02x (%s)", protocol_bitmap,
             protocol_str);
  } else {
    ESP_LOGW(kTag,
             "[driver] esp_wifi_get_protocol failed: %s (expected before "
             "esp_wifi_set_mode())",
             esp_err_to_name(err));
  }

  // [config]: this class never calls esp_wifi_set_storage() itself, so
  // whatever ESP-IDF's own default is applies -- there is no
  // esp_wifi_get_storage() to read it back from the driver.
  ESP_LOGI(kTag,
           "[config] storage_mode: not set explicitly by this class "
           "(ESP-IDF default applies)");
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
  LogDriverStateAfterInit(sta_netif_);

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

  // [config]: exactly what this class is about to hand the driver -- see
  // the header comment on wifi_credentials_ for why threshold/pmf_cfg are
  // left at their zero-initialized defaults (never set explicitly by this
  // class) rather than fabricated here.
  ESP_LOGI(kTag,
           "[config] Configuring Wi-Fi: SSID=\"%s\" password_len=%u "
           "scan_method=%s auth_threshold=%s pmf_required=%d pmf_capable=%d",
           reinterpret_cast<const char*>(wifi_config.sta.ssid),
           static_cast<unsigned>(std::strlen(creds.password)),
           WifiScanMethodToString(wifi_config.sta.scan_method),
           WifiAuthModeToString(wifi_config.sta.threshold.authmode),
           wifi_config.sta.pmf_cfg.required, wifi_config.sta.pmf_cfg.capable);

  err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
    xEventGroupSetBits(events_, kFailedBit);
    return err;
  }

  // [driver]: read back what the driver actually now holds, to catch any
  // truncation/normalization esp_wifi_set_config() might have applied --
  // same fields as the [config] log above, for direct comparison.
  {
    wifi_config_t readback = {};
    const esp_err_t get_err = esp_wifi_get_config(WIFI_IF_STA, &readback);
    if (get_err == ESP_OK) {
      ESP_LOGI(kTag,
               "[driver] esp_wifi_get_config: SSID=\"%s\" password_len=%u "
               "scan_method=%s auth_threshold=%s pmf_required=%d "
               "pmf_capable=%d",
               reinterpret_cast<const char*>(readback.sta.ssid),
               static_cast<unsigned>(
                   std::strlen(reinterpret_cast<const char*>(readback.sta.password))),
               WifiScanMethodToString(readback.sta.scan_method),
               WifiAuthModeToString(readback.sta.threshold.authmode),
               readback.sta.pmf_cfg.required, readback.sta.pmf_cfg.capable);
    } else {
      ESP_LOGW(kTag, "[driver] esp_wifi_get_config failed: %s",
               esp_err_to_name(get_err));
    }
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
    bssid_established_ = false;
    last_authmode_ = WIFI_AUTH_OPEN;
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
    bssid_established_ = false;
    last_authmode_ = WIFI_AUTH_OPEN;
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
  // [driver]/[config] state immediately before esp_wifi_connect() -- see
  // requirement 3 of the diagnostics this was added for. "Wi-Fi already
  // started" isn't a queryable driver flag (no esp_wifi_is_started()
  // exists); it's [config] here because it's derived from this class's own
  // structure instead -- TryConnect() is only ever reached from
  // HandleWifiOrIpEvent()'s WIFI_EVENT_STA_START/STA_DISCONNECTED branches,
  // both of which cannot fire until esp_wifi_start() has already returned
  // ESP_OK (see StartConnecting()), so it is unconditionally true here.
  {
    wifi_mode_t mode = WIFI_MODE_NULL;
    const esp_err_t mode_err = esp_wifi_get_mode(&mode);
    uint8_t primary_channel = 0;
    wifi_second_chan_t second_channel = WIFI_SECOND_CHAN_NONE;
    const esp_err_t channel_err =
        esp_wifi_get_channel(&primary_channel, &second_channel);
    char channel_str[16];
    if (channel_err == ESP_OK) {
      std::snprintf(channel_str, sizeof(channel_str), "%u", primary_channel);
    } else {
      std::snprintf(channel_str, sizeof(channel_str), "unknown");
    }
    ESP_LOGI(kTag, "[driver] before esp_wifi_connect: mode=%s channel=%s",
             mode_err == ESP_OK ? WifiModeToString(mode) : "unknown",
             channel_str);
    ESP_LOGI(kTag,
             "[config] before esp_wifi_connect: storage_mode=default "
             "(never set explicitly) wifi_already_started=true");
  }

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
             "[event] Wi-Fi associated; waiting for IP: ssid=\"%.*s\" "
             "bssid=%02x:%02x:%02x:%02x:%02x:%02x channel=%d authmode=%d (%s)",
             connected_data->ssid_len,
             reinterpret_cast<const char*>(connected_data->ssid),
             connected_data->bssid[0], connected_data->bssid[1],
             connected_data->bssid[2], connected_data->bssid[3],
             connected_data->bssid[4], connected_data->bssid[5],
             connected_data->channel, connected_data->authmode,
             WifiAuthModeToString(connected_data->authmode));
    sta_connected_ = true;
    bssid_established_ = true;
    last_authmode_ = connected_data->authmode;
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    const auto* disconnected_data =
        static_cast<const wifi_event_sta_disconnected_t*>(data);
    // Additional disconnect context, kept as its own line so the existing
    // "Wi-Fi disconnected: reason=..." line below is untouched. rssi/bssid
    // come straight off the event itself ([event]); prior_authmode/
    // prior_bssid_established are this class's own memory of the most
    // recent WIFI_EVENT_STA_CONNECTED this cycle, since the disconnect
    // event carries no auth mode of its own.
    ESP_LOGI(kTag,
             "[event] disconnect context: prior_bssid_established=%d "
             "prior_authmode=%s rssi=%d",
             bssid_established_, WifiAuthModeToString(last_authmode_),
             disconnected_data->rssi);
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
             "[event] Wi-Fi connected (associated + IP acquired): ip=" IPSTR
             " gw=" IPSTR " netmask=" IPSTR,
             IP2STR(&got_ip_data->ip_info.ip),
             IP2STR(&got_ip_data->ip_info.gw),
             IP2STR(&got_ip_data->ip_info.netmask));

    // [driver]: DNS is not part of ip_event_got_ip_t itself -- it's a
    // separate esp_netif query, unlike ip/gw/netmask above.
    esp_netif_dns_info_t dns_main = {};
    esp_netif_dns_info_t dns_backup = {};
    const esp_err_t dns_main_err =
        esp_netif_get_dns_info(sta_netif_, ESP_NETIF_DNS_MAIN, &dns_main);
    const esp_err_t dns_backup_err =
        esp_netif_get_dns_info(sta_netif_, ESP_NETIF_DNS_BACKUP, &dns_backup);
    char dns_main_str[40];
    if (dns_main_err == ESP_OK) {
      std::snprintf(dns_main_str, sizeof(dns_main_str), IPSTR,
                    IP2STR(&dns_main.ip.u_addr.ip4));
    } else {
      std::snprintf(dns_main_str, sizeof(dns_main_str), "unavailable (%s)",
                    esp_err_to_name(dns_main_err));
    }
    char dns_backup_str[40];
    if (dns_backup_err == ESP_OK) {
      std::snprintf(dns_backup_str, sizeof(dns_backup_str), IPSTR,
                    IP2STR(&dns_backup.ip.u_addr.ip4));
    } else {
      std::snprintf(dns_backup_str, sizeof(dns_backup_str), "unavailable (%s)",
                    esp_err_to_name(dns_backup_err));
    }
    ESP_LOGI(kTag, "[driver] DNS: main=%s backup=%s", dns_main_str,
             dns_backup_str);
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
