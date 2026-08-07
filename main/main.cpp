#include "app/app.hpp"
#include "esp_log.h"

namespace {

// Logging-only setup, deliberately the one place in this firmware that
// calls esp_log_level_set() -- everything else keeps using plain
// ESP_LOGx() at whatever level it already did. Our own components stay
// at their default (CONFIG_LOG_DEFAULT_LEVEL == INFO, unchanged); only
// third-party/ESP-IDF components noisy enough to drown out our own
// Wi-Fi/OTA/BLE logs at INFO are turned down to WARN here.
//
// Must run after esp_wifi's own __attribute__((constructor))
// (s_set_default_wifi_log_level() in ESP-IDF's wifi_init.c), which resets
// tag "wifi" to CONFIG_LOG_DEFAULT_LEVEL -- that constructor runs before
// app_main() unconditionally, so calling this first thing in app_main()
// already runs after it and is never overwritten afterward (nothing else
// in this codebase or ESP-IDF calls esp_log_level_set("wifi", ...) once
// running). A tag that isn't compiled into a given build (e.g. esp-tls
// if TLS ever became optional) is simply never logged from -- setting a
// level for it ahead of time is harmless.
void ConfigureLogLevels() {
  esp_log_level_set("wifi", ESP_LOG_WARN);
  esp_log_level_set("esp_netif", ESP_LOG_WARN);
  esp_log_level_set("transport_base", ESP_LOG_WARN);
  esp_log_level_set("HTTP_CLIENT", ESP_LOG_WARN);
  esp_log_level_set("esp-tls", ESP_LOG_WARN);
  esp_log_level_set("NimBLE", ESP_LOG_WARN);
}

}  // namespace

extern "C" void app_main() {
  ConfigureLogLevels();

  static app::App app;
  app.Run();
}
