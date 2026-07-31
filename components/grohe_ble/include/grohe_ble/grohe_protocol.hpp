#pragma once

#include "grohe_ble/ble_manager.hpp"

// Interprets the raw GATT data BleManager reports (see BleCharacteristicEvent)
// as the Grohe Blue application protocol: which characteristic is which,
// what a payload means, and how to log it. Owns no NimBLE types and makes no
// NimBLE calls -- everything here is plain data in, structured log lines out.
// BleManager, by contrast, knows nothing about what these bytes mean; see
// its own class comment for the one narrow exception (recognizing which
// characteristic to auto-subscribe to) and why that's still transport
// policy, not protocol interpretation.
//
// M6 scope: cache both characteristic handles as they're found (only the
// read handle is used this milestone; the write handle is cached now
// because a future milestone needs it, not because anything here uses it
// yet), and log every received payload -- structured as
// {timestamp, responseCode} where it parses as the confirmed
// "timestamp:responseCode" format, raw hex otherwise. No writes, no
// business logic: parsing a payload never changes what this class does
// next, it only changes what gets logged.
namespace grohe_ble {

class GroheProtocol {
 public:
  GroheProtocol() = default;

  // Feeds one characteristic-discovery or notification event, exactly as
  // received from BleManager::PollCharacteristicEvents(). Call from the app
  // task only (matches BleManager's own PollEvents()/PollCharacteristicEvents()
  // contract) -- never from a NimBLE callback.
  void HandleCharacteristicEvent(const BleCharacteristicEvent& event);

 private:
  void HandleFound(const ble_uuid_any_t& uuid, uint16_t val_handle);
  void HandleNotification(uint16_t handle, const uint8_t* payload,
                          size_t payload_len);

  // 0 is not a valid GATT handle (handles start at 1), so it doubles as
  // "not yet found" here, matching the same convention
  // BleManager::pending_subscribe_val_handle_ already uses.
  uint16_t read_char_handle_ = 0;
  uint16_t write_char_handle_ = 0;
};

}  // namespace grohe_ble
