#pragma once

#include <cstddef>

// M15.1: a stable, hardware-derived identity for this specific physical
// dial -- so far the Home Assistant integration has had nothing better
// than the dial's own host/IP to identify it by (see
// homeassistant/custom_components/grohe_dial/config_flow.py's own comment
// on that), which breaks if the IP ever changes without a DHCP
// reservation. The ESP32-C3's Wi-Fi station MAC address is exactly what's
// needed instead: globally unique, burned into efuse at the factory,
// never changes for the life of the device.
//
// Deliberately its own tiny component, not folded into firmware_info/
// (build metadata, a completely different concern -- see that
// component's own header comment) or time_service/wifi_connection.hpp
// (which already reads this same value for its own boot-log line via
// esp_wifi_get_mac(), but only after the Wi-Fi driver is initialized;
// esp_read_mac() below reads the same efuse-derived value without that
// dependency, so this component has no dependency on Wi-Fi having
// started at all).
namespace device_id {

// "aa:bb:cc:dd:ee:ff" + NUL.
inline constexpr size_t kMacStrSize = 18;

// Formats this device's Wi-Fi station MAC address as a lowercase,
// colon-separated string (e.g. "3c:71:bf:12:34:56") into `out`, which
// must be at least kMacStrSize bytes. Returns false (buffer left
// untouched) if the underlying efuse read fails -- this should not
// happen on real hardware (every ESP32-C3 has a factory-programmed base
// MAC), but is not treated as fatal: a caller exposing this over the
// network should simply omit the field rather than crash the request.
[[nodiscard]] bool StationMacString(char* out, size_t out_size);

}  // namespace device_id
