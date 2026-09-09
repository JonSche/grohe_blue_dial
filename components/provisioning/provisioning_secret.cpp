#include "provisioning/provisioning_secret.hpp"

#if __has_include("provisioning/provisioning_secret_local.hpp")
#include "provisioning/provisioning_secret_local.hpp"
#else
#error \
    "Missing components/provisioning/include/provisioning/provisioning_secret_local.hpp -- copy provisioning_secret_local.hpp.example to provisioning_secret_local.hpp and fill in your own provisioning shared secret/token. This file is gitignored and never committed."
#endif

namespace provisioning {

const char* LocalProvisioningSecretProvider::Get() const {
  return kLocalProvisioningSecret;
}

}  // namespace provisioning
