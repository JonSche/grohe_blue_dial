#include "grohe_ble/grohe_auth.hpp"

#include <cstring>

#include "mbedtls/base64.h"
#include "mbedtls/md.h"

namespace grohe_ble {
namespace {
constexpr size_t kSha256DigestSize = 32;
}  // namespace

bool DecodePreSharedKey(const char* base64_key, uint8_t* out, size_t out_size,
                        size_t* out_len) {
  size_t written = 0;
  const int rc = mbedtls_base64_decode(
      out, out_size, &written,
      reinterpret_cast<const unsigned char*>(base64_key),
      std::strlen(base64_key));
  if (rc != 0) {
    return false;
  }
  *out_len = written;
  return true;
}

bool ComputeHmacSha256Base64(const uint8_t* key, size_t key_len,
                             const char* message, char* out,
                             size_t out_size) {
  const mbedtls_md_info_t* md_info =
      mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (md_info == nullptr) {
    return false;
  }

  uint8_t digest[kSha256DigestSize];
  const int hmac_rc = mbedtls_md_hmac(
      md_info, key, key_len, reinterpret_cast<const unsigned char*>(message),
      std::strlen(message), digest);
  if (hmac_rc != 0) {
    return false;
  }

  size_t written = 0;
  const int b64_rc =
      mbedtls_base64_encode(reinterpret_cast<unsigned char*>(out), out_size,
                            &written, digest, sizeof(digest));
  if (b64_rc != 0 || written >= out_size) {
    return false;  // No room left for the NUL terminator below.
  }
  out[written] = '\0';
  return true;
}

}  // namespace grohe_ble
