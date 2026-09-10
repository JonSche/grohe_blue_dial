#include "provisioning/api_secret.hpp"

#if __has_include("provisioning/api_secret_local.hpp")
#include "provisioning/api_secret_local.hpp"
#else
#error \
    "Missing components/provisioning/include/provisioning/api_secret_local.hpp -- copy api_secret_local.hpp.example to api_secret_local.hpp and fill in your own local-API shared secret/token. This file is gitignored and never committed."
#endif

namespace provisioning {

const char* LocalApiSecretProvider::Get() const {
  return kLocalApiSecret;
}

}  // namespace provisioning
