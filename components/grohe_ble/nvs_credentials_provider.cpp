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
// M15.2: a separate key, not folded into StoredCredentials -- this one is
// written by a completely different actor (BleManager/GroheClient, after
// a successful identity probe, not the provisioning endpoint) at a
// completely different time (after a connection, not at provisioning
// time), so a single atomic blob covering both would have to be rewritten
// in full every time either half changes for no benefit -- unlike
// {user_id, pre_shared_key_base64}, which really are one logical unit
// always written together.
constexpr char kAddrKey[] = "appliance_addr";

// On-disk layout for kAddrKey -- fixed-size POD, same "blob, not
// individual fields" pattern as StoredCredentials below, for the same
// atomicity reason.
struct StoredApplianceAddress {
  uint8_t addr_type;
  uint8_t addr[6];
};

// On-disk layout -- fixed-size, POD, written/read as a single blob. See
// NvsCredentialsProvider::Set()'s own comment (grohe_credentials.hpp) for
// why one blob, not two separate keys, is what makes this update behave
// atomically. kMaxCredentialFieldLen (128 bytes) is a generous bound for
// either field (an OIDC "sub" claim is typically a short UUID; the
// observed pre-shared key is well under this too) -- matches
// ota::OtaServer's own token buffer size for the same "generous but
// bounded" reasoning. Shared with Credentials's own field-length limit
// (grohe_credentials.hpp) so a caller validating input before calling
// Set() -- M13.2's local provisioning endpoint -- checks against the
// exact same bound this layout actually enforces.
struct StoredCredentials {
  char user_id[kMaxCredentialFieldLen];
  char pre_shared_key_base64[kMaxCredentialFieldLen];
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

  // M15.2: a missing/unreadable entry here is the ordinary case -- either
  // this dial was provisioned before this feature existed, or Set() above
  // (a *different* Init()-time read, further up this same function) just
  // ran for the first time and has nothing pinned yet either way.
  // BleManager's own bootstrap behavior (connect to the first
  // service-UUID match, then probe) covers this identically to a
  // brand-new provisioning.
  StoredApplianceAddress stored_addr{};
  if (handle->get_blob(kAddrKey, &stored_addr, sizeof(stored_addr)) == ESP_OK) {
    pinned_appliance_address_.has_value = true;
    pinned_appliance_address_.addr_type = stored_addr.addr_type;
    std::memcpy(pinned_appliance_address_.addr, stored_addr.addr,
                sizeof(pinned_appliance_address_.addr));
    ESP_LOGI(kTag, "Using pinned Grohe appliance address from NVS");
  }
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
  // M15.2: erased on the same handle, before the one commit() below, so
  // the credential update and the pin invalidation land atomically
  // together -- see this method's own header comment (grohe_credentials.hpp)
  // for why new credentials must always invalidate any previous pin.
  // ESP_ERR_NVS_NOT_FOUND (nothing was pinned yet) is not a failure here.
  if (err == ESP_OK) {
    const esp_err_t erase_err = handle->erase_item(kAddrKey);
    if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) {
      err = erase_err;
    }
  }
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
  pinned_appliance_address_ = PinnedApplianceAddress{};
  ESP_LOGI(kTag, "Grohe credentials provisioned (user_id=%u bytes, "
           "psk=%u bytes) -- any previously pinned appliance address was "
           "cleared", static_cast<unsigned>(user_id_len),
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
  // M15.2: same handle, same commit() -- see Set()'s own comment on why
  // this always goes together with the credentials themselves.
  const esp_err_t addr_err = handle->erase_item(kAddrKey);
  if (addr_err != ESP_OK && addr_err != ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGE(kTag, "Clear(): erase_item (address) failed: %s",
             esp_err_to_name(addr_err));
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
  pinned_appliance_address_ = PinnedApplianceAddress{};
  ESP_LOGI(kTag, "Provisioned Grohe credentials cleared -- falling back "
           "to local dev credentials");
  return true;
}

bool NvsCredentialsProvider::SetPinnedApplianceAddress(
    const PinnedApplianceAddress& address) {
  esp_err_t open_err = ESP_OK;
  auto handle = nvs::open_nvs_handle(kNamespace, NVS_READWRITE, &open_err);
  if (open_err != ESP_OK) {
    ESP_LOGE(kTag, "SetPinnedApplianceAddress(): nvs::open_nvs_handle "
             "failed: %s", esp_err_to_name(open_err));
    return false;
  }

  esp_err_t err;
  if (address.has_value) {
    StoredApplianceAddress stored{};
    stored.addr_type = address.addr_type;
    std::memcpy(stored.addr, address.addr, sizeof(stored.addr));
    err = handle->set_blob(kAddrKey, &stored, sizeof(stored));
  } else {
    err = handle->erase_item(kAddrKey);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
      err = ESP_OK;  // Already unset -- not a failure.
    }
  }
  if (err == ESP_OK) {
    err = handle->commit();
  }
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "SetPinnedApplianceAddress(): failed to persist: %s",
             esp_err_to_name(err));
    return false;
  }

  pinned_appliance_address_ = address;
  if (address.has_value) {
    ESP_LOGI(kTag, "Pinned Grohe appliance address");
  } else {
    ESP_LOGI(kTag, "Cleared pinned Grohe appliance address");
  }
  return true;
}

}  // namespace grohe_ble
