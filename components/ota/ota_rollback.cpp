#include "ota/ota_rollback.hpp"

#include "esp_log.h"
#include "esp_ota_ops.h"

namespace ota {
namespace {
constexpr char kTag[] = "ota";
}  // namespace

void ConfirmBootValid() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
  if (running != nullptr &&
      esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
      ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
    // Reaching here at all -- running application code, past the rest of
    // this firmware's own startup sequence, not crash-looped during it --
    // is itself the confirmation: this boot is the result of an OTA
    // update that hasn't been confirmed yet, and it's healthy.
    ESP_LOGI(kTag, "Booted from a pending-verify OTA update; confirming");
  }

  const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err != ESP_OK) {
    // Only fails if this app wasn't started via the rollback-tracked path
    // at all -- rollback support disabled at build time, or a very first
    // factory boot -- not a fault condition, nothing to abort over. See
    // CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE's own Kconfig help text.
    ESP_LOGD(kTag, "esp_ota_mark_app_valid_cancel_rollback: %s",
             esp_err_to_name(err));
  }
}

}  // namespace ota
