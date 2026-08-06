#include "ota/ota_manager.hpp"

#include <cstring>

#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"
#include "firmware_info/firmware_info.hpp"
#include "time_service/wifi_connection.hpp"

namespace ota {
namespace {
constexpr char kTag[] = "ota";

// How long CheckForUpdate()/StartUpdate() wait for Wi-Fi before giving up
// -- generous enough for WifiConnection's own several retries (each a
// real connect attempt, not an instant failure) to play out, not so long
// that a genuinely unreachable network hangs the calling task for
// minutes. Wi-Fi is not this component's own concern beyond this one
// timeout value -- see wifi_connection.hpp for the actual retry policy.
constexpr TickType_t kWifiAcquireTimeout = pdMS_TO_TICKS(30'000);

// Releases the shared WifiConnection acquisition on every exit path --
// success or any of StartUpdate()/CheckForUpdate()'s several early
// returns -- without repeating `wifi_connection_.Release();` before each
// one. Constructed unconditionally right after Acquire(), whether it
// returned true or false: WifiConnection's own contract is "Acquire()
// always holds a reference regardless of the outcome; Release() exactly
// once regardless too" -- see wifi_connection.hpp.
class WifiGuard {
 public:
  explicit WifiGuard(time_service::WifiConnection& wifi) : wifi_(wifi) {}
  ~WifiGuard() { wifi_.Release(); }

  WifiGuard(const WifiGuard&) = delete;
  WifiGuard& operator=(const WifiGuard&) = delete;

 private:
  time_service::WifiConnection& wifi_;
};

// Shared by CheckForUpdate() and StartUpdate() -- both need the exact
// same connection setup, just take it to a different depth afterward.
// esp_crt_bundle_attach uses ESP-IDF's own maintained bundle of common CA
// roots (already enabled in this project's sdkconfig for other HTTPS
// use), so this accepts any ordinarily-trusted HTTPS server without a
// project-specific certificate to maintain -- appropriate for "a
// configurable HTTPS URL" with no fixed release host yet.
esp_https_ota_config_t MakeOtaConfig(const esp_http_client_config_t& http_config) {
  esp_https_ota_config_t ota_config = {};
  ota_config.http_config = &http_config;
  return ota_config;
}

esp_http_client_config_t MakeHttpConfig(const char* url) {
  esp_http_client_config_t http_config = {};
  http_config.url = url;
  http_config.crt_bundle_attach = esp_crt_bundle_attach;
  http_config.keep_alive_enable = true;
  return http_config;
}
}  // namespace

OtaManager::OtaManager(time_service::WifiConnection& wifi_connection)
    : wifi_connection_(wifi_connection) {}

void OtaManager::Init() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
  if (running != nullptr &&
      esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
      ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
    // Reaching here at all -- running application code, not crash-looped
    // during startup -- is itself the confirmation: this boot is the
    // result of an OTA update that hasn't been confirmed yet, and it's
    // healthy. Report it once; the next CheckForUpdate()/StartUpdate()
    // call moves State() on from here same as any other boot.
    ESP_LOGI(kTag, "Booted from a pending-verify OTA update; confirming");
    state_.store(OtaState::kComplete);
  }

  const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err != ESP_OK) {
    // Only fails if this app wasn't started via the rollback-tracked
    // path at all (e.g. rollback support wasn't enabled at build time,
    // or this is a very first factory boot) -- not a fault condition,
    // nothing to abort over. See CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE's
    // own Kconfig help text.
    ESP_LOGD(kTag, "esp_ota_mark_app_valid_cancel_rollback: %s", esp_err_to_name(err));
  }
}

esp_err_t OtaManager::Fail(esp_err_t err) {
  last_error_.store(err);
  state_.store(OtaState::kFailed);
  return err;
}

UpdateCheckResult OtaManager::CheckForUpdate(const char* url) {
  UpdateCheckResult result;

  state_.store(OtaState::kConnectingWifi);
  const bool wifi_ok = wifi_connection_.Acquire(kWifiAcquireTimeout);
  WifiGuard wifi_guard(wifi_connection_);
  if (!wifi_ok) {
    ESP_LOGW(kTag, "CheckForUpdate: Wi-Fi not available");
    Fail(ESP_ERR_WIFI_NOT_CONNECT);
    state_.store(OtaState::kIdle);
    return result;
  }

  state_.store(OtaState::kChecking);

  const esp_http_client_config_t http_config = MakeHttpConfig(url);
  const esp_https_ota_config_t ota_config = MakeOtaConfig(http_config);

  esp_https_ota_handle_t handle = nullptr;
  esp_err_t err = esp_https_ota_begin(&ota_config, &handle);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "CheckForUpdate: esp_https_ota_begin failed: %s",
             esp_err_to_name(err));
    Fail(err);
    state_.store(OtaState::kIdle);
    return result;
  }

  esp_app_desc_t new_app_info = {};
  err = esp_https_ota_get_img_desc(handle, &new_app_info);
  // Header-only check: always abort here, never perform()/finish() --
  // this call must never write to flash.
  esp_https_ota_abort(handle);
  state_.store(OtaState::kIdle);

  if (err != ESP_OK) {
    ESP_LOGW(kTag, "CheckForUpdate: esp_https_ota_get_img_desc failed: %s",
             esp_err_to_name(err));
    Fail(err);
    return result;
  }

  std::strncpy(result.remote_version, new_app_info.version,
               sizeof(result.remote_version) - 1);
  result.update_available =
      std::strncmp(new_app_info.version, firmware_info::Version(),
                   sizeof(new_app_info.version)) != 0;
  last_error_.store(ESP_OK);
  return result;
}

esp_err_t OtaManager::StartUpdate(const char* url) {
  progress_.store(-1);
  last_error_.store(ESP_OK);

  state_.store(OtaState::kConnectingWifi);
  const bool wifi_ok = wifi_connection_.Acquire(kWifiAcquireTimeout);
  WifiGuard wifi_guard(wifi_connection_);
  if (!wifi_ok) {
    ESP_LOGE(kTag, "StartUpdate: Wi-Fi not available");
    return Fail(ESP_ERR_WIFI_NOT_CONNECT);
  }

  state_.store(OtaState::kChecking);

  const esp_http_client_config_t http_config = MakeHttpConfig(url);
  const esp_https_ota_config_t ota_config = MakeOtaConfig(http_config);

  esp_https_ota_handle_t handle = nullptr;
  esp_err_t err = esp_https_ota_begin(&ota_config, &handle);
  if (err != ESP_OK) {
    // Covers network failure (host unreachable, DNS, connect timeout)
    // and TLS failure (certificate not trusted by the bundle, TLS
    // handshake error) alike -- esp_https_ota_begin() is where the HTTPS
    // connection is actually established.
    ESP_LOGE(kTag, "StartUpdate: esp_https_ota_begin failed: %s",
             esp_err_to_name(err));
    return Fail(err);
  }

  esp_app_desc_t new_app_info = {};
  err = esp_https_ota_get_img_desc(handle, &new_app_info);
  if (err != ESP_OK) {
    // Covers an HTTP-level error status (esp_https_ota_begin() itself
    // only validates the connection, not the response code) and a
    // malformed/truncated image header alike.
    ESP_LOGE(kTag, "StartUpdate: esp_https_ota_get_img_desc failed: %s",
             esp_err_to_name(err));
    esp_https_ota_abort(handle);
    return Fail(err);
  }

  // Version mismatch, defensively: the same check ESP-IDF's own
  // advanced_https_ota example performs before ever writing a byte to
  // flash. Refusing to "update" to the exact version already running is
  // a deliberate no-op, not a transport failure -- still surfaced as
  // kFailed/ESP_ERR_INVALID_VERSION so a caller can tell the two apart
  // from the returned esp_err_t/LastError() alone.
  if (std::strncmp(new_app_info.version, firmware_info::Version(),
                   sizeof(new_app_info.version)) == 0) {
    ESP_LOGW(kTag,
             "StartUpdate: new image version (%s) matches the running "
             "version -- refusing",
             new_app_info.version);
    esp_https_ota_abort(handle);
    return Fail(ESP_ERR_INVALID_VERSION);
  }

  state_.store(OtaState::kDownloading);
  const int total_size = esp_https_ota_get_image_size(handle);

  esp_err_t perform_err;
  do {
    perform_err = esp_https_ota_perform(handle);
    if (total_size > 0) {
      const int read = esp_https_ota_get_image_len_read(handle);
      progress_.store(static_cast<int>(100LL * read / total_size));
    }
    // total_size <= 0 (chunked transfer, no Content-Length reported):
    // nothing honest to report -- Progress() stays -1 for the whole
    // download rather than a fabricated/stale percentage.
  } while (perform_err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

  if (perform_err != ESP_OK) {
    // Covers an interrupted download (connection dropped mid-stream) and
    // an invalid image (bad magic byte, chip-ID mismatch -- esp_https_ota
    // itself verifies chip ID as part of this loop) alike.
    ESP_LOGE(kTag, "StartUpdate: esp_https_ota_perform failed: %s",
             esp_err_to_name(perform_err));
    esp_https_ota_abort(handle);
    return Fail(perform_err);
  }

  state_.store(OtaState::kVerifying);
  if (!esp_https_ota_is_complete_data_received(handle)) {
    ESP_LOGE(kTag, "StartUpdate: incomplete OTA image received");
    esp_https_ota_abort(handle);
    return Fail(ESP_ERR_OTA_VALIDATE_FAILED);
  }

  state_.store(OtaState::kInstalling);
  // esp_https_ota_finish() frees `handle` unconditionally, success or
  // failure -- esp_https_ota_abort() must never be called after this
  // point. It also only switches the boot partition (esp_ota_set_boot_
  // partition()) if every prior stage, including its own final image
  // validation, succeeded -- so a failure here still leaves the
  // currently running partition untouched and still selected to boot.
  err = esp_https_ota_finish(handle);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "StartUpdate: esp_https_ota_finish failed: %s",
             esp_err_to_name(err));
    return Fail(err);
  }

  progress_.store(100);
  state_.store(OtaState::kRebooting);
  ESP_LOGI(kTag, "OTA update succeeded (%s -> %s); rebooting",
           firmware_info::Version(), new_app_info.version);
  esp_restart();
}

}  // namespace ota
