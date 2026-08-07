#pragma once

// Credentials needed to associate with a Wi-Fi network -- used by
// WifiConnection (see wifi_connection.hpp), the one place this firmware
// brings Wi-Fi up, on behalf of SntpTimeProvider (a one-shot SNTP time
// source). Abstracted behind WifiCredentialsProvider, mirroring
// grohe_ble's Credentials/CredentialsProvider/LocalCredentialsProvider
// exactly, so a future NVS- or Wi-Fi-provisioning-based implementation
// only ever requires writing a new WifiCredentialsProvider and
// constructing it instead of this one -- WifiConnection only ever
// depends on the abstract interface.
namespace time_service {

struct WifiCredentials {
  const char* ssid;
  const char* password;
};

class WifiCredentialsProvider {
 public:
  virtual ~WifiCredentialsProvider() = default;

  [[nodiscard]] virtual const WifiCredentials& Get() const = 0;
};

// Reads credentials from a gitignored local header (wifi_credentials_local.hpp;
// see wifi_credentials_local.hpp.example and .gitignore) -- development-only,
// mirroring grohe_ble's LocalCredentialsProvider and the Python reference
// implementation's own gitignored .env. No secret ever appears in a
// committed file.
class LocalWifiCredentialsProvider final : public WifiCredentialsProvider {
 public:
  [[nodiscard]] const WifiCredentials& Get() const override;
};

}  // namespace time_service
