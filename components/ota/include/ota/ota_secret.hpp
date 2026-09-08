#pragma once

// The shared secret/token that gates the local-network OTA endpoint (see
// ota_server.hpp) -- mirrors grohe_ble::CredentialsProvider and
// time_service::WifiCredentialsProvider exactly (same dependency-injection
// shape, same gitignored-local-header pattern), so a future NVS- or
// provisioning-based implementation only ever requires writing a new
// OtaSecretProvider; OtaServer only ever depends on this interface.
namespace ota {

class OtaSecretProvider {
 public:
  virtual ~OtaSecretProvider() = default;

  // Empty string means "no secret configured". OtaServer treats that as
  // "refuse to start the OTA endpoint at all" -- never as "no auth
  // required" -- see ota_server.cpp.
  [[nodiscard]] virtual const char* Get() const = 0;
};

// Reads the secret from a gitignored local header (ota_secret_local.hpp;
// see ota_secret_local.hpp.example and .gitignore) -- development-only,
// mirroring grohe_ble::LocalCredentialsProvider and
// time_service::LocalWifiCredentialsProvider exactly. No secret ever
// appears in a committed file.
class LocalOtaSecretProvider final : public OtaSecretProvider {
 public:
  [[nodiscard]] const char* Get() const override;
};

}  // namespace ota
