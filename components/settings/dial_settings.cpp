#include "settings/dial_settings.hpp"

#include <cstdint>

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs_handle.hpp"

namespace settings {
namespace {
constexpr char kTag[] = "dial_settings";
constexpr char kNamespace[] = "dial_settings";
constexpr char kKey[] = "values";

// On-disk layout -- fixed-size, POD, written/read as a single blob (see
// DialSettingsStore::Set()'s own comment for why one blob, not three
// separate keys). WaterType is stored as its plain underlying value,
// not the enum itself: NVS has no notion of this project's own enum
// types, and storing the raw value keeps this layout stable even if
// dial_state::WaterType's C++ representation ever changes.
struct StoredSettings {
  int32_t default_amount_ml;
  int32_t amount_step_ml;
  uint8_t default_water_type;
};

[[nodiscard]] bool IsValid(const StoredSettings& stored) {
  if (stored.default_amount_ml < dial_state::kMinAmountMl ||
      stored.default_amount_ml > dial_state::kMaxAmountMl) {
    return false;
  }
  // Upper-bounded by kMaxAmountMl, not some separate constant: a step
  // larger than the whole amount range is never meaningful either way.
  if (stored.amount_step_ml <= 0 || stored.amount_step_ml > dial_state::kMaxAmountMl) {
    return false;
  }
  switch (static_cast<dial_state::WaterType>(stored.default_water_type)) {
    case dial_state::WaterType::kStill:
    case dial_state::WaterType::kMedium:
    case dial_state::WaterType::kSparkling:
      return true;
  }
  return false;
}

StoredSettings ToStored(const DialSettings& values) {
  return StoredSettings{
      .default_amount_ml = values.default_amount_ml,
      .amount_step_ml = values.amount_step_ml,
      .default_water_type = static_cast<uint8_t>(values.default_water_type),
  };
}
}  // namespace

void DialSettingsStore::Init() {
  // values_ already holds dial_state.hpp's own compile-time defaults (see
  // the struct's own in-class initializers) -- everything below only ever
  // *overrides* them, and only with a validated, fully-formed entry.
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "nvs_flash_init failed: %s -- using compile-time defaults",
             esp_err_to_name(err));
    return;
  }

  esp_err_t open_err = ESP_OK;
  auto handle = nvs::open_nvs_handle(kNamespace, NVS_READONLY, &open_err);
  if (open_err != ESP_OK) {
    // ESP_ERR_NVS_NOT_FOUND: namespace never created yet -- the ordinary
    // case on a device nothing has ever been written to, not a real
    // error.
    if (open_err != ESP_ERR_NVS_NOT_FOUND) {
      ESP_LOGW(kTag, "nvs::open_nvs_handle failed: %s -- using compile-time "
               "defaults", esp_err_to_name(open_err));
    }
    return;
  }

  StoredSettings stored{};
  const esp_err_t read_err = handle->get_blob(kKey, &stored, sizeof(stored));
  if (read_err != ESP_OK) {
    if (read_err != ESP_ERR_NVS_NOT_FOUND) {
      ESP_LOGW(kTag, "reading stored settings failed: %s -- using "
               "compile-time defaults", esp_err_to_name(read_err));
    }
    return;
  }

  if (!IsValid(stored)) {
    ESP_LOGW(kTag, "stored settings failed validation -- using "
             "compile-time defaults");
    return;
  }

  values_.default_amount_ml = stored.default_amount_ml;
  values_.amount_step_ml = stored.amount_step_ml;
  values_.default_water_type =
      static_cast<dial_state::WaterType>(stored.default_water_type);
  ESP_LOGI(kTag,
           "Loaded stored settings: default_amount_ml=%d amount_step_ml=%d "
           "default_water_type=%s",
           values_.default_amount_ml, values_.amount_step_ml,
           dial_state::WaterTypeLabel(values_.default_water_type));
}

bool DialSettingsStore::Set(const DialSettings& new_values) {
  const StoredSettings stored = ToStored(new_values);
  if (!IsValid(stored)) {
    ESP_LOGW(kTag, "Set(): rejected out-of-range settings (amount=%d "
             "step=%d)", new_values.default_amount_ml,
             new_values.amount_step_ml);
    return false;
  }

  esp_err_t open_err = ESP_OK;
  auto handle = nvs::open_nvs_handle(kNamespace, NVS_READWRITE, &open_err);
  if (open_err != ESP_OK) {
    ESP_LOGE(kTag, "Set(): nvs::open_nvs_handle failed: %s",
             esp_err_to_name(open_err));
    return false;
  }

  // A single blob write (rather than three separate keys) is what makes
  // this update atomic in practice: NVS's own per-entry CRC means a
  // torn/interrupted write is caught on the next read (get_blob() simply
  // fails, exactly as if nothing had ever been written -- see Init()'s
  // own handling of that), never observed as a mix of old and new field
  // values.
  esp_err_t err = handle->set_blob(kKey, &stored, sizeof(stored));
  if (err == ESP_OK) {
    err = handle->commit();
  }
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "Set(): failed to persist settings: %s",
             esp_err_to_name(err));
    return false;
  }

  values_ = new_values;
  ESP_LOGI(kTag,
           "Settings updated: default_amount_ml=%d amount_step_ml=%d "
           "default_water_type=%s",
           values_.default_amount_ml, values_.amount_step_ml,
           dial_state::WaterTypeLabel(values_.default_water_type));
  return true;
}

}  // namespace settings
