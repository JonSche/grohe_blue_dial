#include "grohe_ble/grohe_credentials.hpp"

#if __has_include("grohe_ble/credentials_local.hpp")
#include "grohe_ble/credentials_local.hpp"
#else
#error \
    "Missing components/grohe_ble/include/grohe_ble/credentials_local.hpp -- copy credentials_local.hpp.example to credentials_local.hpp and fill in your Grohe Blue USER_ID/PRESHARED_KEY (see grohe_blue_ble's own .env.example). This file is gitignored and never committed."
#endif

namespace grohe_ble {

const Credentials& LocalCredentialsProvider::Get() const {
  static constexpr Credentials kCredentials{kLocalUserId,
                                             kLocalPreSharedKeyBase64};
  return kCredentials;
}

}  // namespace grohe_ble
