#pragma once

// The shared secret/token that gates the local HTTP API (M15,
// /api/status, /api/config, /api/dispense, /api/stop -- see
// provisioning_server.hpp) -- mirrors ProvisioningSecretProvider/
// ota::OtaSecretProvider exactly (same DI shape, same
// gitignored-local-header pattern), but is deliberately a *separate*
// secret from both: this one gates an endpoint that can make the
// physical appliance dispense water right now, a materially more
// immediate consequence than either OTA's (flash new firmware) or
// Provisioning's (overwrite stored Grohe credentials, inert until the
// next command) -- worth rotating/scoping independently. ProvisioningServer
// only ever depends on this interface.
namespace provisioning {

class ApiSecretProvider {
 public:
  virtual ~ApiSecretProvider() = default;

  // Empty string means "no secret configured". ProvisioningServer treats
  // that as "refuse to register the /api/* endpoints at all" -- never as
  // "no auth required" -- see provisioning_server.cpp.
  [[nodiscard]] virtual const char* Get() const = 0;
};

// Reads the secret from a gitignored local header (api_secret_local.hpp;
// see api_secret_local.hpp.example and .gitignore) -- development-only,
// mirroring every other Local*Provider in this project exactly. No
// secret ever appears in a committed file.
class LocalApiSecretProvider final : public ApiSecretProvider {
 public:
  [[nodiscard]] const char* Get() const override;
};

}  // namespace provisioning
