#pragma once

#include <functional>

#include "esp_err.h"
#include "grohe_ble/ble_manager.hpp"
#include "grohe_ble/grohe_credentials.hpp"
#include "grohe_ble/grohe_protocol.hpp"
#include "time_service/sntp_time_provider.hpp"
#include "time_service/wifi_credentials.hpp"

namespace grohe_ble {

// The result of a command GroheClient previously sent, consumed exactly
// once via TakeCommandOutcome() -- see that method's own comment for why
// this is edge-triggered rather than a persisted flag.
struct CommandOutcome {
  bool available = false;
  bool was_dispense = false;  // false means this was the stop() command.
  ApplianceState state;
};

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
// M7 introduced GroheClient's sequencing role for a single, automatically-
// fired stop() probe. M8 replaces that with genuine, caller-triggered
// commands: RequestDispense()/RequestStop() send a command (at most one
// outstanding at a time); TakeCommandOutcome() reports how it resolved.
// GroheClient still decides nothing about *why* a command is sent --
// that's app::DialController's job, driven by encoder input -- it only
// owns *whether it's currently possible* (connection/subscribe state) and
// *which of the two in-flight commands a given acknowledgement answers*
// (see CommandOutcome and ApplianceState::sequence), mirroring how the
// Python reference's client.py orchestrates ble.py/protocol.py without
// either of them knowing about sequencing themselves.
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

  // Sends a dispense command for amount_ml/taste. Returns false (no side
  // effect) without sending anything if: a previous command's outcome
  // hasn't been consumed yet (see TakeCommandOutcome()), the connection
  // isn't ready to accept protocol writes yet, or payload building fails.
  // Never retries -- the caller decides whether to try again.
  [[nodiscard]] bool RequestDispense(int amount_ml, WaterType taste);

  // Sends the confirmed stop() command. Same contract as RequestDispense().
  [[nodiscard]] bool RequestStop();

  // Returns and clears the latest command's outcome, if a new one has
  // arrived since the last call -- see CommandOutcome's own comment.
  [[nodiscard]] CommandOutcome TakeCommandOutcome();

  // Whether a valid Unix epoch is currently available (M9) -- surfaced so
  // App/DialController can show a clear "NO TIME" status rather than
  // silently rejecting every command. Does not itself gate SendCommand();
  // BuildDispensePayload()/BuildStopPayload() already reject on this via
  // TimeProvider::GetCurrentEpoch(), this is purely for display.
  [[nodiscard]] bool HasValidTime() const { return time_provider_.IsValid(); }

 private:
  enum class PendingCommand { kNone, kDispense, kStop };

  // Shared by RequestDispense()/RequestStop(): builds and sends the given
  // command's payload, and if the write is successfully queued, records
  // pending_command_ and the ApplianceState sequence at that moment (the
  // baseline TakeCommandOutcome() compares against). amount_ml/taste are
  // meaningless for kStop (BuildStopPayload() ignores them).
  [[nodiscard]] bool SendCommand(PendingCommand kind, int amount_ml,
                                 WaterType taste);

  BleManager ble_manager_;
  GroheProtocol protocol_;
  LocalCredentialsProvider credentials_provider_;

  // M9: time_provider_ is injected with wifi_credentials_provider_ by
  // reference (not owned internally by SntpTimeProvider) so a future
  // WifiCredentialsProvider implementation (NVS, Wi-Fi provisioning, ...)
  // only requires changing this one construction line -- see
  // time_service/wifi_credentials.hpp's own comment. Declared in this
  // order so wifi_credentials_provider_ is fully constructed before
  // time_provider_'s constructor runs (member init order follows
  // declaration order in C++, not the mem-initializer-list order below).
  time_service::LocalWifiCredentialsProvider wifi_credentials_provider_;
  time_service::SntpTimeProvider time_provider_{wifi_credentials_provider_};

  // Gate for SendCommand(): both become true independently and in no
  // guaranteed order (hardware evidence shows ReadyForProtocol can fire
  // before the CCCD subscribe write completes). Reset on
  // kConnectionFailed, matching BleManager's own per-connection reset
  // discipline.
  bool ready_for_protocol_ = false;
  bool subscribed_ = false;

  // At most one command outstanding at a time -- see PendingCommand's own
  // comment and TakeCommandOutcome()'s.
  PendingCommand pending_command_ = PendingCommand::kNone;
  uint32_t pending_command_baseline_sequence_ = 0;
};

}  // namespace grohe_ble
