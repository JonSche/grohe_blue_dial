#pragma once

#include <functional>

#include "esp_err.h"
#include "grohe_ble/ble_manager.hpp"
#include "grohe_ble/grohe_credentials.hpp"
#include "grohe_ble/grohe_protocol.hpp"

namespace grohe_ble {

// The one class app:: is allowed to talk to for BLE -- App never touches
// BleManager directly.
//
// M6: Poll()'s public signature and behavior are exactly what M3.1
// established (drain the lifecycle queue, forward each event to the
// caller's callback) -- App::Run()'s call site does not change at all.
// Internally, Poll() also drains BleManager's separate characteristic
// queue and feeds it to an owned GroheProtocol, which is where the actual
// Grohe protocol interpretation happens (see grohe_protocol.hpp). This is
// the only class that knows both BleManager and GroheProtocol exist.
//
// M7: GroheClient additionally owns the one-time sequencing decision this
// milestone needs -- when it's safe to send the confirmed stop() probe
// (once discovery has finished *and* notifications are subscribed; see
// MaybeSendStopProbe()'s own comment for why both are required) -- mirroring
// how the Python reference's client.py orchestrates ble.py/protocol.py
// without either of them knowing about sequencing themselves. GroheProtocol
// stays a pure encoder/decoder (like protocol.py); BleManager stays
// protocol-agnostic transport (like ble.py); this class is the only one
// that decides *when* to call into either.
class GroheClient {
 public:
  GroheClient() = default;

  esp_err_t Init();

  void Poll(const std::function<void(const BleEvent&)>& on_event);

  // The appliance's latest decoded response, if any -- see
  // GroheProtocol::State()'s own comment.
  [[nodiscard]] const ApplianceState& LatestApplianceState() const {
    return protocol_.State();
  }

 private:
  // Sends the one-time stop() probe once it's safe to (see the member
  // flags below), building the payload from credentials_provider_ and
  // protocol_, then handing it to ble_manager_. No-op otherwise. Called
  // from Poll(), after both queues have been drained for this cycle.
  void MaybeSendStopProbe();

  BleManager ble_manager_;
  GroheProtocol protocol_;
  LocalCredentialsProvider credentials_provider_;

  // Gate for MaybeSendStopProbe(): both become true independently and in
  // no guaranteed order (hardware evidence shows ReadyForProtocol can fire
  // before the CCCD subscribe write completes), so the probe fires once
  // both are true and stays fired for the rest of this connection. All
  // three reset on kConnectionFailed, matching BleManager's own per-
  // connection reset discipline -- a future milestone with reconnect will
  // get a fresh probe per connection "for free" because of this.
  bool ready_for_protocol_ = false;
  bool subscribed_ = false;
  bool stop_probe_sent_ = false;
};

}  // namespace grohe_ble
