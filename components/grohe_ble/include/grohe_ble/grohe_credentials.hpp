#pragma once

#include <cstddef>
#include <cstdint>

// Credentials required to HMAC-sign a Grohe Blue BLE command (see
// grohe_auth.hpp), abstracted behind CredentialsProvider so a future
// milestone can supply them from the cloud API or NVS-backed secure storage
// without any protocol code (grohe_protocol.cpp's BuildStopPayload()) having
// to change -- it only ever depends on this interface. As of M13.1, that
// future milestone is this one: NvsCredentialsProvider below is the
// NVS-backed implementation, sitting alongside LocalCredentialsProvider
// rather than replacing it.
namespace grohe_ble {

// Mirrors the Python reference implementation's GroheCredentials
// (credentials.py): the OpenID Connect "sub" claim used as the BLE user id,
// and the Base64-encoded pre-shared key from GET /v3/iot/dashboard. Plain
// borrowed pointers, not owning strings -- every implementation of
// CredentialsProvider owns its own storage (LocalCredentialsProvider's is a
// pair of static string literals) and must outlive any Credentials it hands
// out.
struct Credentials {
  const char* user_id;
  const char* pre_shared_key_base64;
};

// Maximum length (including the NUL terminator) either Credentials field
// may occupy -- matches NvsCredentialsProvider's own fixed on-disk field
// size (nvs_credentials_provider.cpp) exactly. Exposed here, not kept a
// private implementation detail, so a caller validating input *before*
// handing it to Set() (M13.2's local provisioning endpoint,
// components/provisioning/) can reject an over-long field with a precise
// 400 rather than relying on Set()'s own defensive check, which -- correct
// as it is -- has no way to report anything more specific than "failed".
inline constexpr size_t kMaxCredentialFieldLen = 128;

// M15.2: the BLE address of the specific Grohe Blue Home this dial has
// cryptographically verified accepts commands signed with the current
// Credentials -- see ble_manager.hpp's own SetAddressFilter()/
// ClearAddressFilter() comments for how BleManager uses this, and
// grohe_client.hpp's own comment for how/when it gets set in the first
// place (a one-time probe, never RSSI/scan-order/device-name). Deliberately
// plain bytes, not NimBLE's own ble_addr_t -- this header has no NimBLE
// dependency today and shouldn't gain one just for this; BleManager/
// GroheClient (which already depend on NimBLE) convert at the boundary.
struct PinnedApplianceAddress {
  bool has_value = false;
  uint8_t addr_type = 0;
  uint8_t addr[6] = {};
};

class CredentialsProvider {
 public:
  virtual ~CredentialsProvider() = default;

  [[nodiscard]] virtual const Credentials& Get() const = 0;

  // has_value=false means "no appliance verified yet against the
  // current Credentials" -- the bootstrap state (see ble_manager.hpp's
  // own comment on what BleManager does differently in that state).
  [[nodiscard]] virtual PinnedApplianceAddress GetPinnedApplianceAddress() const = 0;

  // Records a newly cryptographically-verified appliance address.
  // Returns false on a genuine storage failure (NvsCredentialsProvider);
  // LocalCredentialsProvider's own in-memory implementation never fails.
  virtual bool SetPinnedApplianceAddress(const PinnedApplianceAddress& address) = 0;
};

// Reads credentials from a gitignored local header (credentials_local.hpp;
// see credentials_local.hpp.example and .gitignore) -- development-only,
// mirroring the Python reference implementation's own gitignored .env. No
// secret ever appears in a committed file.
class LocalCredentialsProvider final : public CredentialsProvider {
 public:
  [[nodiscard]] const Credentials& Get() const override;

  // In-memory only -- no persistence layer exists for the dev-only
  // credentials this class serves, and none is needed: a reboot during
  // local development re-running from credentials_local.hpp is exactly
  // the "re-bootstrap" case ble_manager.hpp's own comment already
  // describes as safe and expected.
  [[nodiscard]] PinnedApplianceAddress GetPinnedApplianceAddress() const override {
    return pinned_appliance_address_;
  }
  bool SetPinnedApplianceAddress(const PinnedApplianceAddress& address) override {
    pinned_appliance_address_ = address;
    return true;
  }

 private:
  PinnedApplianceAddress pinned_appliance_address_{};
};

// M13.1: credentials provisioned at runtime (M13.2's local HTTP endpoint,
// see components/provisioning/ once that milestone lands), persisted in
// NVS. Get() returns the provisioned pair whenever one is stored and
// valid; otherwise it falls back to an owned LocalCredentialsProvider,
// exactly like before this class existed -- provisioning is additive,
// never a new requirement for the existing developer workflow (an empty/
// missing NVS entry behaves identically to LocalCredentialsProvider alone
// today). Never logs either field's value anywhere in its implementation
// (nvs_credentials_provider.cpp) -- only lengths/success or failure.
class NvsCredentialsProvider final : public CredentialsProvider {
 public:
  NvsCredentialsProvider() = default;

  // Opens NVS (idempotent nvs_flash_init() -- matches this project's own
  // established multi-caller-safe pattern; see e.g.
  // time_service::WifiConnection::Init()) and loads any previously
  // provisioned credentials, if present and valid. Never fails hard: any
  // problem (NVS not ready, nothing provisioned yet, a corrupt/malformed
  // entry) just leaves Get() falling back to the local dev credentials --
  // see the class's own comment. Call once, before Get()/Set().
  void Init();

  [[nodiscard]] const Credentials& Get() const override;

  // Validates both fields (non-empty, fit the fixed on-disk layout --
  // see nvs_credentials_provider.cpp) *before* touching NVS at all, then
  // writes the whole {user_id, preshared_key} pair as a single NVS blob
  // entry and commits it -- see dial_settings.cpp's identical reasoning
  // (components/settings/) for why one blob write, not two separate
  // keys, is what actually delivers "no partially-written credential
  // set". Updates Get() immediately on success, no reboot required.
  // Returns false (NVS and Get() both untouched) if validation fails.
  //
  // M15.2: also clears any previously pinned appliance address (see
  // GetPinnedApplianceAddress()'s own comment) -- new credentials mean
  // the "verified appliance" trust boundary starts over, whether this
  // is first-ever provisioning or the appliance being replaced. Cleared
  // even if the incoming credentials happen to be byte-identical to
  // what was already stored; provisioning is a deliberate user action
  // (the M16 Options Flow, or scripts/provision.sh), not a background
  // event this class should try to optimize around.
  [[nodiscard]] bool Set(const Credentials& new_credentials);

  // Erases the stored entry, if any -- Get() falls back to the local dev
  // credentials again immediately afterward. Returns false only on a
  // genuine NVS failure; erasing an already-empty entry is not an error.
  // Also clears any pinned appliance address, same reasoning as Set()
  // above.
  [[nodiscard]] bool Clear();

  [[nodiscard]] PinnedApplianceAddress GetPinnedApplianceAddress() const override {
    return pinned_appliance_address_;
  }

  // Persisted immediately (single small NVS blob, its own key -- see
  // nvs_credentials_provider.cpp) and cached in memory, same shape as
  // Set() above for the credentials themselves.
  bool SetPinnedApplianceAddress(const PinnedApplianceAddress& address) override;

 private:
  LocalCredentialsProvider local_fallback_;
  bool provisioned_ = false;
  char user_id_[kMaxCredentialFieldLen] = {};
  char pre_shared_key_base64_[kMaxCredentialFieldLen] = {};
  Credentials cached_{};
  PinnedApplianceAddress pinned_appliance_address_{};
};

}  // namespace grohe_ble
