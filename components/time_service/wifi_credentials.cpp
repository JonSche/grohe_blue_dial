#include "time_service/wifi_credentials.hpp"

#if __has_include("time_service/wifi_credentials_local.hpp")
#include "time_service/wifi_credentials_local.hpp"
#else
#error \
    "Missing components/time_service/include/time_service/wifi_credentials_local.hpp -- copy wifi_credentials_local.hpp.example to wifi_credentials_local.hpp and fill in your real Wi-Fi SSID/password. This file is gitignored and never committed."
#endif

namespace time_service {

const WifiCredentials& LocalWifiCredentialsProvider::Get() const {
  static constexpr WifiCredentials kCredentials{kLocalWifiSsid,
                                                kLocalWifiPassword};
  return kCredentials;
}

}  // namespace time_service
