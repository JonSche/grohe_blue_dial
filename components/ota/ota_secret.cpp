#include "ota/ota_secret.hpp"

#if __has_include("ota/ota_secret_local.hpp")
#include "ota/ota_secret_local.hpp"
#else
#error \
    "Missing components/ota/include/ota/ota_secret_local.hpp -- copy ota_secret_local.hpp.example to ota_secret_local.hpp and fill in your own OTA shared secret/token. This file is gitignored and never committed."
#endif

namespace ota {

const char* LocalOtaSecretProvider::Get() const { return kLocalOtaSecret; }

}  // namespace ota
