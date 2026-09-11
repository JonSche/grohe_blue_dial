#include "device_id/device_id.hpp"

#include <cstdio>

#include "esp_mac.h"

namespace device_id {

bool StationMacString(char* out, size_t out_size) {
  if (out == nullptr || out_size < kMacStrSize) {
    return false;
  }
  uint8_t mac[6] = {};
  // ESP_MAC_WIFI_STA: the same station-interface MAC
  // wifi_connection.cpp's own [driver] MAC: log line reports via
  // esp_wifi_get_mac(WIFI_IF_STA, ...) once the Wi-Fi driver is up --
  // both derive it from the same efuse base MAC via the same algorithm
  // (see esp_read_mac()'s own documentation), so the two are guaranteed
  // to agree without this component depending on Wi-Fi having started.
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
    return false;
  }
  std::snprintf(out, out_size, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0],
               mac[1], mac[2], mac[3], mac[4], mac[5]);
  return true;
}

}  // namespace device_id
