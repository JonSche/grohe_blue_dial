#include "grohe_ble/grohe_credentials.hpp"

#include <cstring>

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs_handle.hpp"

namespace grohe_ble {
namespace {
constexpr char kTag[] = "grohe_credentials";
constexpr char kNamespace[] = "grohe_creds";
constexpr char kKey[] = "creds";

// On-disk layout -- fixed-size, POD, written/read as a single blob. See
// NvsCredentialsProvider::Set()'s own comment (grohe_credentials.hpp) for
// why one blob, not two separate keys, is what makes this update behave
// atomically. 128 bytes is a generous bound for either field (an OIDC
// "sub" claim is typically a short UUID; the observed pre-shared key is
// well under this too) -- matches ota::OtaServer's own token buffer size
// for the same "generous but bounded" reasoning.
struct StoredCredentials {
  char user_id[128];
  char pre_shared_key_base64[128];
};
}  // namespace

void NvsCredentialsProvider::Init() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "nvs_flash_init failed: %s -- provisioned credentials "
             "unavailable this boot, falling back to local dev "
             "credentials", esp_err_to_name(err));
    return;
  }

  esp_err_t open_err = ESP_OK;
  auto handle = nvs::open_nvs_handle(kNamespace, NVS_READONLY, &open_err);
  if (open_err != ESP_OK) {
    // ESP_ERR_NVS_NOT_FOUND: never provisioned -- the ordinary case on a
    // fresh device or one still using dev credentials, not a real error.
    if (open_err != ESP_ERR_NVS_NOT_FOUND) {
      ESP_LOGW(kTag, "nvs::open_nvs_handle failed: %s -- falling back to "
               "local dev credentials", esp_err_to_name(open_err));
    }
    return;
  }

  StoredCredentials stored{};
  const esp_err_t read_err = handle->get_blob(kKey, &stored, sizeof(stored));
  if (read_err != ESP_OK) {
    if (read_err != ESP_ERR_NVS_NOT_FOUND) {
      ESP_LOGW(kTag, "reading provisioned credentials failed: %s -- "
               "falling back to local dev credentials",
               esp_err_to_name(read_err));
    }
    return;
  }

  // Defensive: NVS's own per-entry CRC already catches a torn/corrupt
  // blob (get_blob() would have failed above) -- this only guards
  // against a stored entry that's *valid* NVS but empty at the
  // application level, which Set() below never itself writes but is
  // still worth checking rather than trusting blindly.
  stored.user_id[sizeof(stored.user_id) - 1] = '\0';
  stored.pre_shared_key_base64[sizeof(stored.pre_shared_key_base64) - 1] = '\0';
  if (stored.user_id[0] == '\0' || stored.pre_shared_key_base64[0] == '\0') {
    ESP_LOGW(kTag, "stored credentials entry is empty/malformed -- "
             "falling back to local dev credentials");
    return;
  }

  std::memcpy(user_id_, stored.user_id, sizeof(user_id_));
  std::memcpy(pre_shared_key_base64_, stored.pre_shared_key_base64,
              sizeof(pre_shared_key_base64_));
  cached_ = Credentials{user_id_, pre_shared_key_base64_};
  provisioned_ = true;
  // Never logs either field's value -- lengths/success only.
  ESP_LOGI(kTag, "Using provisioned Grohe credentials from NVS");
}

const Credentials& NvsCredentialsProvider::Get() const {
  if (provisioned_) {
    return cached_;
  }
  return local_fallback_.Get();
}

bool NvsCredentialsProvider::Set(const Credentials& new_credentials) {
  if (new_credentials.user_id == nullptr ||
      new_credentials.pre_shared_key_base64 == nullptr) {
    ESP_LOGW(kTag, "Set(): rejected -- null field");
    return false;
  }
  const size_t user_id_len = std::strlen(new_credentials.user_id);
  const size_t psk_len = std::strlen(new_credentials.pre_shared_key_base64);
  if (user_id_len == 0 || user_id_len >= sizeof(StoredCredentials::user_id) ||
      psk_len == 0 ||
      psk_len >= sizeof(StoredCredentials::pre_shared_key_base64)) {
    // Field values are never logged -- lengths only, which is exactly
    // what a caller needs to tell "empty" from "too long" apart.
    ESP_LOGW(kTag, "Set(): rejected -- empty or too long (user_id=%u "
             "bytes, psk=%u bytes)", static_cast<unsigned>(user_id_len),
             static_cast<unsigned>(psk_len));
    return false;
  }

  esp_err_t open_err = ESP_OK;
  auto handle = nvs::open_nvs_handle(kNamespace, NVS_READWRITE, &open_err);
  if (open_err != ESP_OK) {
    ESP_LOGE(kTag, "Set(): nvs::open_nvs_handle failed: %s",
             esp_err_to_name(open_err));
    return false;
  }

  StoredCredentials stored{};
  std::strncpy(stored.user_id, new_credentials.user_id,
               sizeof(stored.user_id) - 1);
  std::strncpy(stored.pre_shared_key_base64,
               new_credentials.pre_shared_key_base64,
               sizeof(stored.pre_shared_key_base64) - 1);

  // A single blob write (rather than two separate keys) is what makes
  // this update atomic in practice -- see the class's own comment in
  // grohe_credentials.hpp.
  esp_err_t err = handle->set_blob(kKey, &stored, sizeof(stored));
  if (err == ESP_OK) {
    err = handle->commit();
  }
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "Set(): failed to persist credentials: %s",
             esp_err_to_name(err));
    return false;
  }

  std::memcpy(user_id_, stored.user_id, sizeof(user_id_));
  std::memcpy(pre_shared_key_base64_, stored.pre_shared_key_base64,
              sizeof(pre_shared_key_base64_));
  cached_ = Credentials{user_id_, pre_shared_key_base64_};
  provisioned_ = true;
  ESP_LOGI(kTag, "Grohe credentials provisioned (user_id=%u bytes, "
           "psk=%u bytes)", static_cast<unsigned>(user_id_len),
           static_cast<unsigned>(psk_len));
  return true;
}

bool NvsCredentialsProvider::Clear() {
  esp_err_t open_err = ESP_OK;
  auto handle = nvs::open_nvs_handle(kNamespace, NVS_READWRITE, &open_err);
  if (open_err != ESP_OK) {
    ESP_LOGE(kTag, "Clear(): nvs::open_nvs_handle failed: %s",
             esp_err_to_name(open_err));
    return false;
  }
  esp_err_t err = handle->erase_item(kKey);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGE(kTag, "Clear(): erase_item failed: %s", esp_err_to_name(err));
    return false;
  }
  err = handle->commit();
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "Clear(): commit failed: %s", esp_err_to_name(err));
    return false;
  }

  provisioned_ = false;
  cached_ = Credentials{};
  std::memset(user_id_, 0, sizeof(user_id_));
  std::memset(pre_shared_key_base64_, 0, sizeof(pre_shared_key_base64_));
  ESP_LOGI(kTag, "Provisioned Grohe credentials cleared -- falling back "
           "to local dev credentials");
  return true;
}

}  // namespace grohe_ble
