#include "provisioning/provisioning_server.hpp"

#include <cstring>

#include "cJSON.h"
#include "esp_log.h"

namespace provisioning {
namespace {
constexpr char kTag[] = "provisioning";
constexpr char kAuthHeader[] = "X-Provision-Token";
// Generous upper bound for a header value -- large enough for any
// reasonable shared secret, small enough to stay a trivial stack buffer.
// Matches ota::OtaServer's own kMaxTokenLen exactly (same reasoning,
// different secret).
constexpr size_t kMaxTokenLen = 128;

// The whole request body is just {"user_id": "...", "preshared_key_base64":
// "..."} -- generous enough for two grohe_ble::kMaxCredentialFieldLen-sized
// fields plus JSON punctuation/whitespace, small enough to read in one
// shot with no streaming/chunking needed (contrast with OTA's firmware
// upload, which is megabytes and deliberately never buffered in full --
// see ota_server.hpp). Also doubles as this endpoint's request-size guard:
// anything larger is rejected before a single byte of the body is read.
constexpr size_t kMaxBodyLen = 512;

// OTA's esp_http_server instance already owns port 80 (HTTPD_DEFAULT_
// CONFIG()'s own default -- see ota_server.cpp) and components/ota/ is
// unmodified by this milestone, so provisioning gets its own instance on
// its own port rather than sharing OTA's. Plain, unencrypted HTTP -- the
// same "no TLS, local network only" posture as OTA, for the identical
// reason (this ESP32-C3 has no PSRAM and mbedTLS's own handshake
// allocation doesn't fit alongside Wi-Fi+BLE+LVGL -- see
// docs/ARCHITECTURE.md's "OTA (M12)" section for the hardware evidence).
constexpr uint16_t kProvisioningPort = 8080;

// Simple constant-time comparison -- identical reasoning and shape to
// ota_server.cpp's own helper of the same name, duplicated rather than
// shared so components/provisioning/ has no dependency on
// components/ota/ at all (see this milestone's own "do not touch OTA"
// constraint).
bool ConstantTimeEquals(const char* a, const char* b) {
  const size_t len_a = std::strlen(a);
  const size_t len_b = std::strlen(b);
  const size_t max_len = len_a > len_b ? len_a : len_b;
  uint32_t diff = static_cast<uint32_t>(len_a != len_b);
  for (size_t i = 0; i < max_len; ++i) {
    const uint8_t byte_a = i < len_a ? static_cast<uint8_t>(a[i]) : 0;
    const uint8_t byte_b = i < len_b ? static_cast<uint8_t>(b[i]) : 0;
    diff |= static_cast<uint32_t>(byte_a ^ byte_b);
  }
  return diff == 0;
}

// Bridges esp_http_server's plain-function-pointer httpd_uri_t::handler to
// a ProvisioningServer instance via req->user_ctx -- the same trampoline
// shape already used for OTA (see FlushCbTrampoline,
// components/display/gc9a01_display.cpp, and OtaPostTrampoline,
// components/ota/ota_server.cpp).
esp_err_t ProvisionPostTrampoline(httpd_req_t* req) {
  return static_cast<ProvisioningServer*>(req->user_ctx)->HandleProvisionPost(req);
}
}  // namespace

ProvisioningServer::ProvisioningServer(
    time_service::WifiConnection& wifi_connection,
    const ProvisioningSecretProvider& secret_provider,
    grohe_ble::NvsCredentialsProvider& credentials_provider)
    : wifi_connection_(wifi_connection),
      secret_provider_(secret_provider),
      credentials_provider_(credentials_provider) {}

ProvisioningServer::~ProvisioningServer() {
  if (server_ != nullptr) {
    httpd_stop(server_);
  }
  // Deliberately never calls wifi_connection_.Release(): Init() (below)
  // acquires one reference for the entire process lifetime, and this
  // object is never destroyed before process exit in practice -- App owns
  // it for the lifetime of app_main(), which never returns (see app.hpp).
}

void ProvisioningServer::Init() {
  const char* secret = secret_provider_.Get();
  if (secret == nullptr || secret[0] == '\0') {
    ESP_LOGW(kTag,
             "No provisioning secret configured "
             "(provisioning_secret_local.hpp) -- provisioning endpoint "
             "disabled; the existing local dev credentials are unaffected");
    return;
  }

  // A second permanent consumer of WifiConnection alongside ota::OtaServer
  // -- this reference is held for the whole process lifetime (Release()
  // is never called), matching this milestone's own "permanently
  // reachable whenever Wi-Fi is connected, no button/gesture gating"
  // requirement. Non-blocking: app startup (display, encoder, BLE)
  // proceeds immediately regardless of how long Wi-Fi takes to associate,
  // or whether it ever does -- provisioning is not a runtime dependency,
  // the same principle as every other WifiConnection consumer.
  wifi_connection_.AcquireAsync(
      [this]() { StartServer(); },
      [this]() {
        ESP_LOGW(kTag,
                 "Wi-Fi did not connect -- provisioning endpoint "
                 "unavailable this boot; USB/local credential fallback is "
                 "unaffected");
      });
}

void ProvisioningServer::StartServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = kProvisioningPort;
  config.max_uri_handlers = 1;
  config.max_open_sockets = 2;
  config.lru_purge_enable = true;

  const esp_err_t err = httpd_start(&server_, &config);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "httpd_start failed: %s", esp_err_to_name(err));
    server_ = nullptr;
    return;
  }

  const httpd_uri_t provision_uri = {
      .uri = "/provision",
      .method = HTTP_POST,
      .handler = &ProvisionPostTrampoline,
      .user_ctx = this,
  };
  httpd_register_uri_handler(server_, &provision_uri);

  ESP_LOGI(kTag,
           "Provisioning endpoint ready on port %u: POST /provision (%s "
           "header required) -- see the Wi-Fi connection log above for "
           "this device's IP",
           static_cast<unsigned>(kProvisioningPort), kAuthHeader);
}

bool ProvisioningServer::IsAuthorized(httpd_req_t* req) const {
  const char* expected = secret_provider_.Get();
  // Init() already refused to start the server at all if this is empty
  // -- defensive, not reachable in practice.
  if (expected == nullptr || expected[0] == '\0') {
    return false;
  }

  char received[kMaxTokenLen] = {};
  const esp_err_t err = httpd_req_get_hdr_value_str(req, kAuthHeader, received,
                                                      sizeof(received));
  if (err != ESP_OK) {
    // ESP_ERR_NOT_FOUND (header missing) or ESP_ERR_HTTPD_RESULT_TRUNC
    // (value longer than any real token this project uses) alike -- both
    // are simply "not authorized", never logged with the value.
    return false;
  }

  return ConstantTimeEquals(received, expected);
}

esp_err_t ProvisioningServer::HandleProvisionPost(httpd_req_t* req) {
  // Authenticate before reading or parsing the body at all -- matches the
  // flow this mirrors in provisioning_server.hpp's own comment and
  // ota_server.cpp's identical ordering for /ota.
  if (!IsAuthorized(req)) {
    ESP_LOGW(kTag, "Provisioning request rejected: missing or invalid %s "
             "header", kAuthHeader);
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    return ESP_FAIL;
  }

  if (req->content_len == 0 || req->content_len > kMaxBodyLen) {
    ESP_LOGW(kTag, "Provisioning request rejected: body length %u out of "
             "bounds (max %u)", static_cast<unsigned>(req->content_len),
             static_cast<unsigned>(kMaxBodyLen));
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body size");
    return ESP_FAIL;
  }

  // Read the whole body in one go -- see kMaxBodyLen's own comment for why
  // no streaming/chunking is needed here, unlike OTA's firmware upload.
  char body[kMaxBodyLen + 1];
  size_t received_total = 0;
  while (received_total < req->content_len) {
    const int received = httpd_req_recv(req, body + received_total,
                                        req->content_len - received_total);
    if (received == HTTPD_SOCK_ERR_TIMEOUT) {
      continue;  // Retry -- matches ota_server.cpp's own httpd_req_recv() loop.
    }
    if (received <= 0) {
      ESP_LOGW(kTag, "Provisioning request rejected: read failed (%d)",
               received);
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Read failed");
      return ESP_FAIL;
    }
    received_total += static_cast<size_t>(received);
  }
  body[received_total] = '\0';

  cJSON* json = cJSON_ParseWithLength(body, received_total);
  if (json == nullptr) {
    ESP_LOGW(kTag, "Provisioning request rejected: malformed JSON");
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Malformed JSON");
    return ESP_FAIL;
  }

  // Field values are read but never logged anywhere in this function --
  // only whether they're present/valid, and (on rejection) their length.
  const cJSON* user_id_item = cJSON_GetObjectItemCaseSensitive(json, "user_id");
  const cJSON* psk_item =
      cJSON_GetObjectItemCaseSensitive(json, "preshared_key_base64");

  const bool types_ok = cJSON_IsString(user_id_item) &&
                        user_id_item->valuestring != nullptr &&
                        cJSON_IsString(psk_item) &&
                        psk_item->valuestring != nullptr;

  size_t user_id_len = 0;
  size_t psk_len = 0;
  bool lengths_ok = false;
  if (types_ok) {
    user_id_len = std::strlen(user_id_item->valuestring);
    psk_len = std::strlen(psk_item->valuestring);
    // < kMaxCredentialFieldLen, not <=: the stored field must still fit a
    // NUL terminator -- the exact same bound
    // grohe_ble::NvsCredentialsProvider::Set() itself enforces (see that
    // function's own comment), checked here first so a too-long field is
    // reported as 400 (a bad request), not 500 (implying a server-side
    // fault it isn't).
    lengths_ok = user_id_len > 0 && user_id_len < grohe_ble::kMaxCredentialFieldLen &&
                psk_len > 0 && psk_len < grohe_ble::kMaxCredentialFieldLen;
  }

  if (!types_ok || !lengths_ok) {
    cJSON_Delete(json);
    ESP_LOGW(kTag, "Provisioning request rejected: missing, empty, "
             "non-string, or too-long user_id/preshared_key_base64 "
             "(user_id=%u bytes, psk=%u bytes)",
             static_cast<unsigned>(user_id_len), static_cast<unsigned>(psk_len));
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                         "Missing or invalid user_id/preshared_key_base64");
    return ESP_FAIL;
  }

  // NvsCredentialsProvider::Set() validates again on its own (defense in
  // depth -- this endpoint is not the only caller in principle) and only
  // ever writes the complete, validated pair as a single atomic NVS blob:
  // an invalid request is rejected above, before this call, and a request
  // that fails inside Set() (a genuine NVS-level failure, not a bad
  // request) leaves whatever was already stored completely untouched --
  // see that function's own comment for why.
  const grohe_ble::Credentials new_credentials{user_id_item->valuestring,
                                               psk_item->valuestring};
  const bool stored = credentials_provider_.Set(new_credentials);
  cJSON_Delete(json);  // Must outlive the Set() call above: valuestring
                       // pointers into it are read by Set(), not copied
                       // until after it returns.

  if (!stored) {
    ESP_LOGE(kTag, "Provisioning failed: writing credentials to NVS "
             "failed -- existing credentials, if any, are untouched");
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                         "Failed to persist credentials");
    return ESP_FAIL;
  }

  ESP_LOGI(kTag, "Grohe credentials provisioned via HTTP");
  httpd_resp_set_type(req, "application/json");
  // NvsCredentialsProvider::Set() already updates its own in-memory cache
  // on success (see that function's own comment) -- the very next BLE
  // command GroheClient sends signs with the new credentials, connection
  // already established or not, no reboot required. reboot_required is
  // reported explicitly (not just implied by omission) so a caller never
  // has to guess.
  static constexpr char kSuccessBody[] =
      "{\"status\":\"ok\",\"reboot_required\":false,"
      "\"message\":\"Credentials stored; active immediately for the next "
      "BLE command.\"}\n";
  httpd_resp_send(req, kSuccessBody, sizeof(kSuccessBody) - 1);
  return ESP_OK;
}

}  // namespace provisioning
