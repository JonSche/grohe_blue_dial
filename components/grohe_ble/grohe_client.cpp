#include "grohe_ble/grohe_client.hpp"

#include <cstring>

#include "esp_log.h"

namespace grohe_ble {
namespace {
constexpr char kTag[] = "grohe_client";
}  // namespace

GroheClient::GroheClient(time_service::WifiConnection& wifi_connection,
                         CredentialsProvider& credentials_provider)
    : credentials_provider_(credentials_provider),
      time_provider_(wifi_connection) {}

esp_err_t GroheClient::Init() {
  // Non-fatal, same reasoning as BleManager's own failures below: a Wi-Fi/
  // SNTP problem must not take down BLE, the display, or the encoder. Time
  // and BLE are independent, so order between the two Init() calls doesn't
  // matter functionally -- starting the (non-blocking, event-driven) time
  // sync first just gives it the most possible head start before a command
  // is ever likely to be sent.
  const esp_err_t time_err = time_provider_.Init();
  if (time_err != ESP_OK) {
    ESP_LOGE(kTag, "SntpTimeProvider::Init() failed: %s",
             esp_err_to_name(time_err));
  }
  const esp_err_t ble_err = ble_manager_.Init();
  if (ble_err != ESP_OK) {
    return ble_err;
  }

  // M15.2: applies any appliance pin already on disk (a normal reboot of
  // an already-verified dial) *before* this task ever calls Poll() --
  // SetAddressFilter() is queue-based and safe to call this early (the
  // queue itself is created inside the Init() call just above, host task
  // startup/first scan happen asynchronously after that either way). No
  // pin yet (first boot after provisioning, or firmware predating M15.2
  // reusing an older credentials store) leaves discovery exactly as it
  // was before this milestone -- connect to the first service-UUID
  // match, then MaybeStartIdentityProbe() (via Poll(), once subscribed)
  // verifies and pins it.
  const PinnedApplianceAddress pinned =
      credentials_provider_.GetPinnedApplianceAddress();
  if (pinned.has_value) {
    ble_addr_t addr{};
    addr.type = pinned.addr_type;
    std::memcpy(addr.val, pinned.addr, sizeof(addr.val));
    ble_manager_.SetAddressFilter(addr);
  }
  return ESP_OK;
}

void GroheClient::Poll(const std::function<void(const BleEvent&)>& on_event) {
  ble_manager_.PollEvents([this, &on_event](const BleEvent& event) {
    on_event(event);
    switch (event.type) {
      case BleEventType::kDeviceFound:
        // M15.2: recorded here, not read synchronously from BleManager
        // later -- see last_found_address_'s own comment.
        last_found_address_ = event.peer_address;
        break;
      case BleEventType::kReadyForProtocol:
        ready_for_protocol_ = true;
        break;
      case BleEventType::kSubscribed:
        subscribed_ = true;
        break;
      case BleEventType::kConnectionFailed:
        // A fresh connection -- BleManager retries automatically as of
        // M11.1 -- starts this sequence over -- see the members' own
        // comment in grohe_client.hpp.
        ready_for_protocol_ = false;
        subscribed_ = false;
        pending_command_ = PendingCommand::kNone;
        break;
      default:
        break;
    }
  });
  // M15.2: called every cycle, not just once on the kSubscribed event
  // above -- see this method's own comment for why a single-shot
  // attempt isn't robust enough (real-hardware evidence: kSubscribed
  // can fire before SNTP has synced, which would otherwise mean this
  // connection's one and only probe attempt fails and a perfectly
  // legitimate appliance is never pinned this connection). Cheap and
  // quiet to call repeatedly -- its own early-return guards make every
  // call after the first genuine attempt a no-op.
  MaybeStartIdentityProbe();
  ble_manager_.PollCharacteristicEvents(
      [this](const BleCharacteristicEvent& event) {
        protocol_.HandleCharacteristicEvent(event);
      });
  // After PollCharacteristicEvents() above, so protocol_'s state already
  // reflects any notification that arrived this cycle -- the response to
  // a probe sent *this* cycle (by the MaybeStartIdentityProbe() call
  // just above) cannot possibly have arrived yet regardless.
  ProcessIdentityProbeOutcome();
}

bool GroheClient::RequestDispense(int amount_ml, WaterType taste) {
  return SendCommand(PendingCommand::kDispense, amount_ml, taste);
}

bool GroheClient::RequestStop() {
  return SendCommand(PendingCommand::kStop, 0, WaterType::kUnknown);
}

bool GroheClient::SendCommand(PendingCommand kind, int amount_ml,
                              WaterType taste) {
  if (pending_command_ != PendingCommand::kNone) {
    ESP_LOGW(kTag, "command already in flight, ignoring new request");
    return false;
  }
  if (!ready_for_protocol_ || !subscribed_) {
    ESP_LOGW(kTag, "not ready for protocol commands yet");
    return false;
  }
  const uint16_t write_handle = protocol_.write_char_handle();
  if (write_handle == 0) {
    // Structurally not expected on this hardware (M5 confirmed the write
    // characteristic is always present), but nothing here should assume
    // that rather than check it.
    ESP_LOGE(kTag, "write characteristic never found");
    return false;
  }

  char payload[kMaxStopPayloadSize];
  // kIdentityProbe (M15.2) is, on the wire, exactly a stop() command --
  // see PendingCommand's own comment on why that specific command, not a
  // new one, is what gets sent.
  const bool is_stop_shaped =
      kind == PendingCommand::kStop || kind == PendingCommand::kIdentityProbe;
  const bool built =
      is_stop_shaped
          ? BuildStopPayload(credentials_provider_.Get(), time_provider_,
                             payload, sizeof(payload))
          : BuildDispensePayload(credentials_provider_.Get(), amount_ml,
                                 taste, time_provider_, payload,
                                 sizeof(payload));
  if (!built) {
    // Covers both an HMAC/buffer failure and "no valid time available yet"
    // (M9) -- BuildDispensePayload()/BuildStopPayload() don't distinguish
    // the reason at this level, and this class already rejects a command
    // the same way regardless of which it was.
    ESP_LOGE(kTag, "failed to build command payload");
    return false;
  }

  const size_t payload_len = std::strlen(payload);
  const esp_err_t err = ble_manager_.WriteCharacteristic(
      write_handle, reinterpret_cast<const uint8_t*>(payload), payload_len);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "WriteCharacteristic failed: %s", esp_err_to_name(err));
    return false;
  }

  const char* kind_str = kind == PendingCommand::kIdentityProbe ? "identity-probe"
                        : kind == PendingCommand::kStop          ? "stop"
                                                                  : "dispense";
  ESP_LOGI(kTag, "%s command queued (%u bytes)", kind_str,
          static_cast<unsigned>(payload_len));
  pending_command_ = kind;
  pending_command_baseline_sequence_ = protocol_.State().sequence;
  return true;
}

void GroheClient::MaybeStartIdentityProbe() {
  if (!ready_for_protocol_ || !subscribed_) {
    // Called every Poll() cycle (see Poll()'s own comment) -- this is
    // the common, quiet case while still connecting/discovering
    // services, not a failure worth logging.
    return;
  }
  if (credentials_provider_.GetPinnedApplianceAddress().has_value) {
    return;  // Already verified -- BleManager's own address filter is
             // what keeps this connection pinned; nothing to probe.
  }
  if (pending_command_ != PendingCommand::kNone) {
    return;  // A probe (or, in principle, another command) is already
             // in flight -- SendCommand() would refuse a second one
             // anyway, checked here too purely to skip the attempt
             // (and its log line below) entirely rather than let
             // SendCommand() log its own "already in flight" warning
             // every single cycle while one is genuinely outstanding.
  }
  // Real-hardware evidence (this milestone's own acceptance testing):
  // kSubscribed can fire before SNTP has finished syncing, which makes
  // BuildStopPayload() (inside SendCommand()) fail -- M9's own "no
  // command without a valid clock" rule, not specific to this probe.
  // Retried automatically on the next cycle (this method itself is now
  // called every Poll() cycle, not just once on the kSubscribed edge --
  // see Poll()'s own comment) rather than the probe getting exactly one
  // shot and silently never pinning a perfectly legitimate appliance
  // just because it happened to connect unusually fast this boot.
  if (!SendCommand(PendingCommand::kIdentityProbe, 0, WaterType::kUnknown)) {
    // DEBUG, not WARN: SendCommand() already logged the specific reason
    // at its own appropriate level, and a single transient failure here
    // is expected/retried, not actionable on its own.
    ESP_LOGD(kTag, "M15.2: identity probe not sent this cycle, will retry");
    return;
  }
  ESP_LOGI(kTag, "M15.2: no appliance pinned yet -- identity probe sent "
           "to the currently-connected candidate");
}

void GroheClient::ProcessIdentityProbeOutcome() {
  if (pending_command_ != PendingCommand::kIdentityProbe) {
    return;
  }
  const ApplianceState& state = protocol_.State();
  if (!state.received || state.sequence == pending_command_baseline_sequence_) {
    return;  // No new response yet.
  }
  pending_command_ = PendingCommand::kNone;

  // Mirrors grohe_protocol.cpp's own ResponseCodeToString() case 1 --
  // no named ResponseCode enum exists in this codebase to reference
  // instead (response_code is deliberately a raw, unenumerated `long`
  // throughout ApplianceState/the protocol layer -- see that struct's
  // own comment on "unknown codes preserve their raw integer value").
  constexpr long kInvalidHmacResponseCode = 1;

  if (!state.is_success && state.response_code == kInvalidHmacResponseCode) {
    ESP_LOGW(kTag, "M15.2: identity probe rejected (INVALID_HMAC) -- this "
             "is not the provisioned Grohe Blue Home; disconnecting and "
             "continuing to search");
    ble_manager_.RejectCurrentPeerAndKeepScanning();
    return;
  }

  // Any other response -- success or even a different, non-HMAC
  // rejection (TIMESTAMP_EXPIRED, GUEST_MODE_DISABLED, ...) -- still
  // proves the appliance accepted this command as genuinely signed by
  // the current pre_shared_key_base64: only the real provisioned
  // appliance can verify that HMAC. See this method's own header
  // comment (grohe_client.hpp) for why that's the one cryptographic
  // identity proof this protocol actually offers.
  ESP_LOGI(kTag, "M15.2: identity probe accepted (response_code=%ld) -- "
           "pinning this appliance's address", state.response_code);
  PinnedApplianceAddress pinned{};
  pinned.has_value = true;
  pinned.addr_type = last_found_address_.type;
  std::memcpy(pinned.addr, last_found_address_.val, sizeof(pinned.addr));
  if (!credentials_provider_.SetPinnedApplianceAddress(pinned)) {
    ESP_LOGE(kTag, "M15.2: failed to persist the newly-verified appliance "
             "address -- will re-probe on the next reconnect");
    return;
  }
  ble_manager_.SetAddressFilter(last_found_address_);
}

CommandOutcome GroheClient::TakeCommandOutcome() {
  CommandOutcome outcome;
  if (pending_command_ == PendingCommand::kNone) {
    return outcome;  // available == false
  }
  const ApplianceState& state = protocol_.State();
  if (!state.received || state.sequence == pending_command_baseline_sequence_) {
    return outcome;  // No new response since this command was sent.
  }

  outcome.available = true;
  outcome.was_dispense = (pending_command_ == PendingCommand::kDispense);
  outcome.state = state;
  pending_command_ = PendingCommand::kNone;
  return outcome;
}

}  // namespace grohe_ble
