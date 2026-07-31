#pragma once

#include <cstdint>

#include "grohe_ble/ble_manager.hpp"
#include "grohe_ble/grohe_credentials.hpp"
#include "time_service/time_provider.hpp"

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
//
// M8 scope: BuildStopPayload() is now a thin wrapper around a general
// BuildDispensePayload(), the same payload-building path a real dispense
// command uses (amount_ml/taste instead of 0/0) -- mirroring protocol.py's
// own create_request(), which both dispense() and stop() go through in the
// Python reference. ApplianceState gains a monotonic `sequence` so
// GroheClient can tell *which* command a given acknowledgement answers
// (see grohe_client.hpp's CommandOutcome) without either side needing to
// re-derive that from the response's own fields, which carry no such
// marker. PredictDispenseDurationMs() ports the Python reference's own
// empirically-measured physical-dispense-duration model -- see its own
// comment for the source and validated range.
//
// M9 scope: BuildDispensePayload()/BuildStopPayload() no longer take a raw
// timestamp -- they take a time_service::TimeProvider& and call
// GetCurrentEpoch() on it themselves. This is the one place GroheProtocol
// touches time_service, and only through its abstract interface (see
// time_provider.hpp) -- it has no idea whether the real implementation is
// SNTP-based, NVS-backed, or anything else. A time-unavailable result is
// handled exactly like an HMAC/buffer failure already was: return false,
// leave `out` untouched, no fabricated timestamp.
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
  // Increments every time HandleNotification successfully parses a new
  // response (M8). The response format itself carries no marker of which
  // command it answers, so consumers that need to (GroheClient, to tell a
  // dispense ack from a stop ack) compare this against a baseline recorded
  // at send time, rather than each independently diffing every field.
  uint32_t sequence = 0;
};

// Mirrors the Python reference's WaterType IntEnum (constants.py) exactly --
// the numeric "taste" field in the BLE payload.
enum class WaterType {
  kUnknown = 0,
  kStill = 1,
  kMedium = 2,
  kSparkling = 3,
  kHot = 4,
  kHotMixed = 5,
};

// The largest payload BuildDispensePayload() (or BuildStopPayload()) may
// produce -- tied to BleCharacteristicEvent's own MTU-derived bound so a
// single GATT write can never exceed what this connection's negotiated MTU
// actually allows, not an independently chosen number that could silently
// drift out of sync with it.
inline constexpr size_t kMaxStopPayloadSize =
    BleCharacteristicEvent::kMaxPayloadSize;

// Builds a signed dispense-command payload -- protocol.py's create_request()
// (in turn DispenseCommand.hmac_message + serialize_payload()):
// HMAC-SHA256-signed "userId:timestamp:amountMl:taste:base64(hmac)". Returns
// false (leaving `out` untouched) if any stage fails: the message or final
// payload doesn't fit in the given buffers, credential decoding/HMAC
// computation fails (see grohe_auth.hpp), or `time_provider` has no valid
// time available (see time_provider.hpp -- never fabricated) -- never
// partially writes `out` in that case. This is the one payload-building
// path for both a real dispense and stop() (see BuildStopPayload() below)
// -- there is no separate, duplicated serialization for either.
[[nodiscard]] bool BuildDispensePayload(const Credentials& credentials,
                                        int amount_ml, WaterType taste,
                                        const time_service::TimeProvider& time_provider,
                                        char* out, size_t out_size);

// The confirmed stop command -- protocol.py's stop_command(): amount=0,
// taste=0, otherwise identical to BuildDispensePayload().
[[nodiscard]] bool BuildStopPayload(const Credentials& credentials,
                                    const time_service::TimeProvider& time_provider,
                                    char* out, size_t out_size);

// Predicts physical dispense duration (from the SUCCESS acknowledgement to
// the appliance fully stopping), in milliseconds, for a given amount.
// Ported directly from the Python reference's own empirically-measured
// model (grohe_blue_ble/docs/PERFORMANCE.md's "Physical Dispense Duration"
// experiment, examples/dispense_duration_test.py,
// results/dispense_duration.csv) -- not re-derived or approximated here:
//
//     dispense_time ≈ startup_overhead + amount_ml * time_per_ml
//
// with startup_overhead ≈ 1.32 s and time_per_ml ≈ 0.0403 s/ml, measured
// across 100-1000 ml. The source document itself notes this is not
// validated outside that range (very small amounts, where startup effects
// may dominate differently, or amounts above 1000 ml, untested).
[[nodiscard]] uint32_t PredictDispenseDurationMs(int amount_ml);

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
  // GroheClient uses this to know when it's safe to build and send a
  // command.
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
