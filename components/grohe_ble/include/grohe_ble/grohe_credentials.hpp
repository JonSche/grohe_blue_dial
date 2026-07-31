#pragma once

// Credentials required to HMAC-sign a Grohe Blue BLE command (see
// grohe_auth.hpp), abstracted behind CredentialsProvider so a future
// milestone can supply them from the cloud API or NVS-backed secure storage
// without any protocol code (grohe_protocol.cpp's BuildStopPayload()) having
// to change -- it only ever depends on this interface.
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

class CredentialsProvider {
 public:
  virtual ~CredentialsProvider() = default;

  [[nodiscard]] virtual const Credentials& Get() const = 0;
};

// Reads credentials from a gitignored local header (credentials_local.hpp;
// see credentials_local.hpp.example and .gitignore) -- development-only,
// mirroring the Python reference implementation's own gitignored .env. No
// secret ever appears in a committed file.
class LocalCredentialsProvider final : public CredentialsProvider {
 public:
  [[nodiscard]] const Credentials& Get() const override;
};

}  // namespace grohe_ble
