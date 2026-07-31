#pragma once

#include <cstdint>

#include "grohe_ble/ble_manager.hpp"
#include "grohe_ble/grohe_credentials.hpp"

// Interprets the raw GATT data BleManager reports (see BleCharacteristicEvent)
// as the Grohe Blue application protocol: which characteristic is which,
// what a payload means, and how to log it. Owns no NimBLE types and makes no
// NimBLE calls -- everything here is plain data in, structured log lines out.
// BleManager, by contrast, knows nothing about what these bytes mean; see
// its own class comment for the one narrow exception (recognizing which
// characteristic to auto-subscribe to) and why that's still transport
// policy, not protocol interpretation.
//
// M6 scope: cache both characteristic handles as they're found, and log
// every received payload -- structured as {timestamp, responseCode} where
// it parses as the confirmed "timestamp:responseCode" format, raw hex
// otherwise.
//
// M7 scope: hardware validation established that this read characteristic
// only ever carries data as an acknowledgement to a write -- there is no
// passive appliance-state broadcast (see docs/ARCHITECTURE.md's "Appliance
// state (M7)" section for the full evidence trail). GroheProtocol now also
// exposes that decoded acknowledgement structurally (ApplianceState, updated
// from HandleNotification whenever a payload parses) and builds the one
// payload GroheClient is allowed to send: the confirmed, idempotent stop()
// command (BuildStopPayload()), used solely to elicit that acknowledgement.
// Building the payload needs the HMAC/credentials machinery from
// grohe_auth.hpp/grohe_credentials.hpp -- both pure, BLE-independent
// modules, mirroring how protocol.py imports auth.py in the Python
// reference. GroheProtocol still makes no NimBLE calls and has no opinion on
// *when* to send anything -- that sequencing lives in GroheClient, which
// owns both this class and BleManager (see grohe_client.hpp).
namespace grohe_ble {

// The appliance's one confirmed protocol response, decoded from a
// successfully parsed "timestamp:responseCode" notification (see
// TryParseResponse() in grohe_protocol.cpp, ported from protocol.py's
// parse_response()/ApplianceResponse). `received` is false until at least
// one such payload has been decoded; an unparseable payload (still logged
// as hex, per M6's rule) never sets it and never overwrites a previously-
// decoded state, since there is nothing else confirmed to replace it with.
struct ApplianceState {
  bool received = false;
  long timestamp = 0;
  long response_code = 0;
  bool is_success = false;
};

// The largest payload BuildStopPayload() (or any future write this protocol
// module builds) may produce -- tied to BleCharacteristicEvent's own MTU-
// derived bound so a single GATT write can never exceed what this
// connection's negotiated MTU actually allows, not an independently chosen
// number that could silently drift out of sync with it.
inline constexpr size_t kMaxStopPayloadSize =
    BleCharacteristicEvent::kMaxPayloadSize;

// Builds the confirmed stop-command payload -- protocol.py's stop_command()
// (amount=0, taste=0) plus serialize_payload(): HMAC-SHA256-signed
// "userId:timestamp:0:0:base64(hmac)". Returns false (leaving `out`
// untouched) if any stage fails: the message or final payload doesn't fit
// in the given buffers, or credential decoding/HMAC computation fails (see
// grohe_auth.hpp) -- never partially writes `out` in that case.
[[nodiscard]] bool BuildStopPayload(const Credentials& credentials,
                                    uint32_t timestamp, char* out,
                                    size_t out_size);

class GroheProtocol {
 public:
  GroheProtocol() = default;

  // Feeds one characteristic-discovery or notification event, exactly as
  // received from BleManager::PollCharacteristicEvents(). Call from the app
  // task only (matches BleManager's own PollEvents()/PollCharacteristicEvents()
  // contract) -- never from a NimBLE callback.
  void HandleCharacteristicEvent(const BleCharacteristicEvent& event);

  // The appliance's latest decoded response, if any (see ApplianceState's
  // own comment for exactly what "latest" means).
  [[nodiscard]] const ApplianceState& State() const { return state_; }

  // 0 until the write characteristic has been discovered (see HandleFound());
  // GroheClient uses this to know when it's safe to build and send the stop
  // probe.
  [[nodiscard]] uint16_t write_char_handle() const {
    return write_char_handle_;
  }

 private:
  void HandleFound(const ble_uuid_any_t& uuid, uint16_t val_handle);
  void HandleNotification(uint16_t handle, const uint8_t* payload,
                          size_t payload_len);

  // 0 is not a valid GATT handle (handles start at 1), so it doubles as
  // "not yet found" here, matching the same convention
  // BleManager::pending_subscribe_val_handle_ already uses.
  uint16_t read_char_handle_ = 0;
  uint16_t write_char_handle_ = 0;
  ApplianceState state_{};
};

}  // namespace grohe_ble
