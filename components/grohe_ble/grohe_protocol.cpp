#include "grohe_ble/grohe_protocol.hpp"

#include <cstdlib>
#include <cstring>

#include "esp_log.h"
#include "grohe_ble/ble_constants.hpp"
#include "host/ble_uuid.h"

namespace grohe_ble {
namespace {
constexpr char kTag[] = "grohe_protocol";

// "aa" per byte + NUL, generous for the largest possible notification
// payload (BleCharacteristicEvent::kMaxPayloadSize).
constexpr size_t kHexBufSize = 2 * BleCharacteristicEvent::kMaxPayloadSize + 1;

void FormatHex(const uint8_t* data, size_t len, char* out, size_t out_size) {
  static constexpr char kHexDigits[] = "0123456789abcdef";
  size_t pos = 0;
  for (size_t i = 0; i < len && pos + 2 < out_size; ++i) {
    out[pos++] = kHexDigits[data[i] >> 4];
    out[pos++] = kHexDigits[data[i] & 0x0f];
  }
  out[pos] = '\0';
}

// The one response format confirmed by the Python reference implementation
// (protocol.py's parse_response()/ApplianceResponse): "timestamp:responseCode",
// exactly two ':'-separated integer fields. Ported directly, not
// re-derived -- see the module-level comment in grohe_protocol.hpp.
struct ParsedResponse {
  long timestamp;
  long code;
};

// Mirrors protocol.py's ResponseCode IntEnum. Unknown codes are logged as
// their raw number, matching parse_response()'s own "unknown codes preserve
// their raw integer value instead of raising" behavior -- this is
// interpreting the one confirmed, already-known field format, not "unknown
// protocol fields".
[[nodiscard]] const char* ResponseCodeToString(long code) {
  switch (code) {
    case 0:
      return "SUCCESS";
    case 1:
      return "INVALID_HMAC";
    case 2:
      return "TIMESTAMP_EXPIRED";
    case 3:
      return "GUEST_MODE_DISABLED";
    case 4:
      return "APPLIANCE_INTERNAL_ERROR";
    default:
      return nullptr;
  }
}

// Returns whether `text` (exactly `len` bytes, not necessarily
// NUL-terminated -- it's a raw BLE payload) is exactly two ':'-separated
// integer fields, and if so, parses them into *out. A payload that doesn't
// match is not an error (see grohe_protocol.hpp): the caller falls back to
// logging it as hex.
[[nodiscard]] bool TryParseResponse(const uint8_t* text, size_t len,
                                    ParsedResponse* out) {
  // Payloads this small don't need a general "no fixed maximum" parser;
  // this bound just keeps the local copy on the stack.
  if (len == 0 || len >= BleCharacteristicEvent::kMaxPayloadSize) {
    return false;
  }
  char buf[BleCharacteristicEvent::kMaxPayloadSize];
  std::memcpy(buf, text, len);
  buf[len] = '\0';

  const char* colon = static_cast<const char*>(std::memchr(buf, ':', len));
  if (colon == nullptr) {
    return false;
  }
  const size_t first_len = static_cast<size_t>(colon - buf);
  const size_t second_len = len - first_len - 1;
  if (first_len == 0 || second_len == 0) {
    return false;
  }
  // Reject a second ':' -- the confirmed format is exactly two fields.
  if (std::memchr(colon + 1, ':', second_len) != nullptr) {
    return false;
  }

  char* end = nullptr;
  const long timestamp = std::strtol(buf, &end, 10);
  if (end != colon) {
    return false;  // Not every character of the first field was a digit.
  }
  const long code = std::strtol(colon + 1, &end, 10);
  if (end != buf + len) {
    return false;  // Not every character of the second field was a digit.
  }

  out->timestamp = timestamp;
  out->code = code;
  return true;
}

// Logs one packet in the required structured form: direction, characteristic
// UUID, length, hex, and -- if it parsed -- the structured
// {timestamp, responseCode} form alongside it. Timestamp-of-log-line comes
// from ESP_LOG's own prefix, matching every other log line in this
// codebase; there is no separate application-level clock to add.
void LogPacket(const char* direction, const ble_uuid_t& uuid,
              const uint8_t* payload, size_t len) {
  char uuid_str[BLE_UUID_STR_LEN];
  ble_uuid_to_str(&uuid, uuid_str);
  char hex[kHexBufSize];
  FormatHex(payload, len, hex, sizeof(hex));

  ParsedResponse parsed;
  if (TryParseResponse(payload, len, &parsed)) {
    const char* code_name = ResponseCodeToString(parsed.code);
    ESP_LOGI(kTag,
             "%s uuid=%s len=%u hex=%s parsed={timestamp=%ld code=%ld%s%s}",
             direction, uuid_str, static_cast<unsigned>(len), hex,
             parsed.timestamp, parsed.code, code_name != nullptr ? " " : "",
             code_name != nullptr ? code_name : "");
  } else {
    ESP_LOGI(kTag, "%s uuid=%s len=%u hex=%s", direction, uuid_str,
             static_cast<unsigned>(len), hex);
  }
}
}  // namespace

void GroheProtocol::HandleCharacteristicEvent(
    const BleCharacteristicEvent& event) {
  switch (event.kind) {
    case BleCharacteristicEventKind::kFound:
      HandleFound(event.uuid, event.val_handle);
      break;
    case BleCharacteristicEventKind::kNotification:
      HandleNotification(event.notify_handle, event.payload,
                         event.payload_len);
      break;
  }
}

void GroheProtocol::HandleFound(const ble_uuid_any_t& uuid,
                                uint16_t val_handle) {
  if (ble_uuid_cmp(&uuid.u, &kGroheReadCharUuid.u) == 0) {
    read_char_handle_ = val_handle;
    ESP_LOGI(kTag, "Read characteristic handle cached: %u", val_handle);
  } else if (ble_uuid_cmp(&uuid.u, &kGroheWriteCharUuid.u) == 0) {
    write_char_handle_ = val_handle;
    ESP_LOGI(kTag, "Write characteristic handle cached: %u", val_handle);
  }
  // Any other characteristic (0x1800/0x1801's Generic Access/Attribute
  // characteristics) is not part of the Grohe protocol -- already logged by
  // BleManager's own discovery trace, nothing further to do here.
}

void GroheProtocol::HandleNotification(uint16_t handle,
                                       const uint8_t* payload,
                                       size_t payload_len) {
  if (read_char_handle_ != 0 && handle != read_char_handle_) {
    // Not fatal -- see grohe_protocol.hpp: a payload this milestone doesn't
    // expect is logged, not treated as a transport error. The appliance
    // only has one characteristic subscribed to, so this would mean NimBLE
    // itself reported a handle this code never asked for.
    ESP_LOGW(kTag, "notification on unexpected handle=%u (expected %u)",
             handle, read_char_handle_);
  }
  LogPacket("RX", kGroheReadCharUuid.u, payload, payload_len);
}

}  // namespace grohe_ble
