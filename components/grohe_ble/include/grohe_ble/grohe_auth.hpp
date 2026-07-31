#pragma once

#include <cstddef>
#include <cstdint>

// HMAC-SHA256 + Base64 helpers for the Grohe Blue BLE protocol, ported
// directly from the Python reference implementation's auth.py: the
// pre-shared key from GET /v3/iot/dashboard is Base64-encoded and must be
// decoded before use; the appliance HMAC is HMAC-SHA256 keyed by that
// decoded key, and the resulting digest is Base64-encoded again before
// being placed in the BLE write payload (see grohe_protocol.hpp's
// BuildStopPayload()). Deliberately free of BLE/credential-provider
// knowledge, mirroring auth.py's own "intentionally free of any BLE or
// cloud logic" scoping -- these are pure functions over plain bytes.
namespace grohe_ble {

// Decodes a Base64 pre-shared key into raw bytes. Returns false (and leaves
// *out_len unspecified) if the input isn't valid Base64 or the decoded
// result doesn't fit in out_size bytes -- callers must check the return
// value rather than assume a fixed key length, since none is confirmed by
// evidence.
[[nodiscard]] bool DecodePreSharedKey(const char* base64_key, uint8_t* out,
                                     size_t out_size, size_t* out_len);

// Returns the Base64-encoded HMAC-SHA256 of `message`, keyed by `key`
// (key_len bytes -- the decoded pre-shared key from DecodePreSharedKey()).
// Returns false if the result (plus a NUL terminator) doesn't fit in
// out_size bytes; `out` is NUL-terminated on success.
[[nodiscard]] bool ComputeHmacSha256Base64(const uint8_t* key, size_t key_len,
                                          const char* message, char* out,
                                          size_t out_size);

}  // namespace grohe_ble
