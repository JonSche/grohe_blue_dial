#pragma once

// The shared secret/token that gates the local-network provisioning
// endpoint (see provisioning_server.hpp) -- mirrors ota::OtaSecretProvider
// exactly (same DI shape, same gitignored-local-header pattern), but is
// deliberately a *separate* secret: a leaked/misused OTA token must never
// also grant the ability to overwrite this dial's Grohe BLE credentials,
// and vice versa. ProvisioningServer only ever depends on this interface.
namespace provisioning {

class ProvisioningSecretProvider {
 public:
  virtual ~ProvisioningSecretProvider() = default;

  // Empty string means "no secret configured". ProvisioningServer treats
  // that as "refuse to start the provisioning endpoint at all" -- never
  // as "no auth required" -- see provisioning_server.cpp.
  [[nodiscard]] virtual const char* Get() const = 0;
};

// Reads the secret from a gitignored local header
// (provisioning_secret_local.hpp; see provisioning_secret_local.hpp.example
// and .gitignore) -- development-only, mirroring
// ota::LocalOtaSecretProvider/grohe_ble::LocalCredentialsProvider/
// time_service::LocalWifiCredentialsProvider exactly. No secret ever
// appears in a committed file.
class LocalProvisioningSecretProvider final : public ProvisioningSecretProvider {
 public:
  [[nodiscard]] const char* Get() const override;
};

}  // namespace provisioning
