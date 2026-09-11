#pragma once

#include <functional>

#include "esp_err.h"
#include "grohe_ble/ble_manager.hpp"
#include "grohe_ble/grohe_credentials.hpp"
#include "grohe_ble/grohe_protocol.hpp"
// sntp_time_provider.hpp transitively includes wifi_connection.hpp, which
// is where time_service::WifiConnection (used below, by reference) is
// actually declared.
#include "time_service/sntp_time_provider.hpp"

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
  // wifi_connection/credentials_provider must both outlive this object
  // (dependency injection, not owned instances -- both live at the
  // composition root, app::App, rather than being constructed internally
  // here). wifi_connection is forwarded straight through to
  // time_provider_ -- this class has no other use for it; see
  // wifi_connection.hpp's own comment and
  // docs/ARCHITECTURE.md#wifi-connectivity. credentials_provider (M13.1)
  // replaces what used to be a hardcoded LocalCredentialsProvider member
  // -- app::App now decides which CredentialsProvider implementation
  // (grohe_ble::LocalCredentialsProvider or the NVS-backed
  // grohe_ble::NvsCredentialsProvider) this client actually uses, the
  // same shape as every other injected dependency in this codebase.
  // Non-const as of M15.2 (was `const CredentialsProvider&` through
  // M13.1-M16): ProcessIdentityProbeOutcome() below needs to call the
  // mutating SetPinnedApplianceAddress() on this exact same instance
  // app::App's provisioning path (components/provisioning/) already
  // mutates via Set()/Clear() -- both are the *same* trust boundary
  // (see grohe_credentials.hpp's own comment on why Set() itself clears
  // the pin), just written from two different tasks/call sites.
  GroheClient(time_service::WifiConnection& wifi_connection,
             CredentialsProvider& credentials_provider);

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
  // M15.2: kIdentityProbe is SendCommand()'s third kind -- a stop()
  // command sent automatically, internally, never by a caller (see
  // MaybeStartIdentityProbe()). Deliberately reuses stop(): it's the one
  // command this protocol already lets a client send speculatively with
  // zero physical side effect ("either does nothing, if nothing's
  // dispensing, or stops an in-progress dispense -- never starts one"),
  // so a probe can never itself cause water to flow -- and the
  // appliance's own HMAC verification of it is exactly the one
  // cryptographic identity check this protocol actually offers (see
  // this class's own comment above). Its outcome is intercepted and
  // consumed entirely inside ProcessIdentityProbeOutcome() -- never
  // surfaced through the public TakeCommandOutcome(), so App/
  // DialController never even know a probe happened.
  enum class PendingCommand { kNone, kDispense, kStop, kIdentityProbe };

  // Shared by RequestDispense()/RequestStop()/MaybeStartIdentityProbe():
  // builds and sends the given command's payload, and if the write is
  // successfully queued, records pending_command_ and the ApplianceState
  // sequence at that moment (the baseline TakeCommandOutcome()/
  // ProcessIdentityProbeOutcome() compares against). amount_ml/taste are
  // meaningless for kStop/kIdentityProbe (BuildStopPayload() ignores
  // them).
  [[nodiscard]] bool SendCommand(PendingCommand kind, int amount_ml,
                                 WaterType taste);

  // M15.2: called on every Poll() cycle once ready_for_protocol_ and
  // subscribed_ are both true (not just once, on the kSubscribed event
  // itself -- real-hardware evidence found the connection can reach
  // that point before SNTP has synced, which fails the one-shot attempt
  // an earlier version of this method made; see grohe_client.cpp's own
  // comment) -- a no-op unless credentials_provider_ has no pinned
  // appliance address yet (the bootstrap state; see
  // grohe_credentials.hpp's own comment on PinnedApplianceAddress) and
  // no probe is already in flight, in which case it (re-)attempts
  // exactly one kIdentityProbe SendCommand(kStop, ...) -- there is at
  // most ever one in-flight command of any kind (the same invariant
  // SendCommand() already enforces for RequestDispense()/RequestStop()),
  // so this and a real user command can never race.
  void MaybeStartIdentityProbe();

  // M15.2: called at the end of every Poll(), after
  // PollCharacteristicEvents() has had a chance to feed protocol_ a new
  // notification -- a no-op unless pending_command_ == kIdentityProbe
  // and a new response has actually arrived (same "sequence changed"
  // detection TakeCommandOutcome() uses). response_code == 1
  // ("INVALID_HMAC" -- grohe_protocol.cpp's own ResponseCodeToString())
  // means this connection is NOT the provisioned appliance:
  // ble_manager_.RejectCurrentPeerAndKeepScanning() disconnects it and
  // resumes the search, never touching credentials_provider_. Any other
  // received response proves the opposite (the appliance accepted a
  // command signed with the current pre_shared_key_base64, which only
  // the genuine provisioned appliance can do) --
  // credentials_provider_.SetPinnedApplianceAddress(last_found_address_)
  // persists it and ble_manager_.SetAddressFilter() applies it
  // immediately, both from the exact address BleEvent::kDeviceFound
  // carried for this same connection (see that field's own comment).
  void ProcessIdentityProbeOutcome();

  BleManager ble_manager_;
  GroheProtocol protocol_;
  CredentialsProvider& credentials_provider_;

  // M9: time_provider_ needs a Wi-Fi connection purely as a one-shot SNTP
  // time source. That connection (time_service::WifiConnection) is
  // injected from outside (the constructor parameter above) rather than
  // owned here, so any future sibling of this class could share the
  // exact same connection rather than run a second, independent one
  // against the one physical Wi-Fi radio this chip has. See
  // wifi_connection.hpp's own comment and
  // docs/ARCHITECTURE.md#wifi-connectivity.
  time_service::SntpTimeProvider time_provider_;

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

  // M15.2: the address BleEvent::kDeviceFound most recently carried --
  // by the time kSubscribed fires for that same connection attempt (the
  // event MaybeStartIdentityProbe() reacts to), this is already set, so
  // ProcessIdentityProbeOutcome() has the right address to pin without
  // needing a synchronous getter into BleManager's own internal state
  // (see BleEvent::peer_address's own comment for why that's the
  // deliberate design, not an oversight).
  ble_addr_t last_found_address_ = {};
};

}  // namespace grohe_ble
