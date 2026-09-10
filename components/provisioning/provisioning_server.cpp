#include "provisioning/provisioning_server.hpp"

#include <cstring>

#include "cJSON.h"
#include "esp_log.h"
#include "mem_diag/mem_diag.hpp"

namespace provisioning {
namespace {
constexpr char kTag[] = "provisioning";
constexpr char kProvisionAuthHeader[] = "X-Provision-Token";
constexpr char kApiAuthHeader[] = "X-Api-Token";
// Generous upper bound for a header value -- large enough for any
// reasonable shared secret, small enough to stay a trivial stack buffer.
// Matches ota::OtaServer's own kMaxTokenLen exactly (same reasoning,
// different secret).
constexpr size_t kMaxTokenLen = 128;

// The whole request body for /provision or any /api/* POST is a small,
// flat JSON object -- generous enough for two
// grohe_ble::kMaxCredentialFieldLen-sized fields plus JSON punctuation/
// whitespace (the largest body this endpoint ever sees), small enough
// to read in one shot with no streaming/chunking needed (contrast with
// OTA's firmware upload, which is megabytes and deliberately never
// buffered in full -- see ota_server.hpp). Also doubles as every POST
// handler's own request-size guard: anything larger is rejected before
// a single byte of the body is read.
constexpr size_t kMaxBodyLen = 512;

// OTA's esp_http_server instance already owns port 80 (HTTPD_DEFAULT_
// CONFIG()'s own default -- see ota_server.cpp) and components/ota/ is
// unmodified by either the original provisioning milestone or M15, so
// this component gets its own instance on its own port rather than
// sharing OTA's. Plain, unencrypted HTTP -- the same "no TLS, local
// network only" posture as OTA, for the identical reason (this
// ESP32-C3 has no PSRAM and mbedTLS's own handshake allocation doesn't
// fit alongside Wi-Fi+BLE+LVGL -- see docs/ARCHITECTURE.md's "OTA
// (M12)" section for the hardware evidence).
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
esp_err_t ApiStatusGetTrampoline(httpd_req_t* req) {
  return static_cast<ProvisioningServer*>(req->user_ctx)->HandleApiStatusGet(req);
}
esp_err_t ApiConfigGetTrampoline(httpd_req_t* req) {
  return static_cast<ProvisioningServer*>(req->user_ctx)->HandleApiConfigGet(req);
}
esp_err_t ApiConfigPostTrampoline(httpd_req_t* req) {
  return static_cast<ProvisioningServer*>(req->user_ctx)->HandleApiConfigPost(req);
}
esp_err_t ApiDispensePostTrampoline(httpd_req_t* req) {
  return static_cast<ProvisioningServer*>(req->user_ctx)->HandleApiDispensePost(req);
}
esp_err_t ApiStopPostTrampoline(httpd_req_t* req) {
  return static_cast<ProvisioningServer*>(req->user_ctx)->HandleApiStopPost(req);
}

// JSON field naming for the /api/* endpoints -- deliberately kept local
// to this file, not added to dial_state.hpp: dial_state::DialState stays
// the single, minimal, HTTP-agnostic source of truth (see its own
// comment); how it gets rendered as JSON for one particular consumer is
// this consumer's own concern, matching dial_state::WaterTypeLabel()'s
// existing precedent of naming being kept next to what actually needs
// it -- reused directly below, not reinvented.
const char* ConnectionStatusJson(dial_state::ConnectionStatus status) {
  switch (status) {
    case dial_state::ConnectionStatus::kConnecting:
      return "CONNECTING";
    case dial_state::ConnectionStatus::kReady:
      return "READY";
    case dial_state::ConnectionStatus::kConnectionLost:
      return "CONNECTION_LOST";
  }
  return "";
}
const char* TimeStatusJson(dial_state::TimeStatus status) {
  switch (status) {
    case dial_state::TimeStatus::kSyncing:
      return "SYNCING";
    case dial_state::TimeStatus::kAvailable:
      return "AVAILABLE";
    case dial_state::TimeStatus::kUnavailable:
      return "UNAVAILABLE";
  }
  return "";
}
const char* DispenseStatusJson(dial_state::DispenseStatus status) {
  switch (status) {
    case dial_state::DispenseStatus::kIdle:
      return "IDLE";
    case dial_state::DispenseStatus::kDispensing:
      return "DISPENSING";
    case dial_state::DispenseStatus::kStopping:
      return "STOPPING";
    case dial_state::DispenseStatus::kFinished:
      return "FINISHED";
    case dial_state::DispenseStatus::kFailed:
      return "FAILED";
  }
  return "";
}

// Parses one of dial_state::WaterTypeLabel()'s own exact output strings
// back into the enum -- the one inverse of that existing function this
// project needed (the physical UI only ever consumes the enum -> string
// direction). Case-sensitive, exact match only -- no fuzzy/lowercase
// acceptance, so a client's typo fails loudly (422) rather than silently
// mapping to the wrong water type.
bool ParseWaterType(const char* text, dial_state::WaterType* out) {
  for (dial_state::WaterType type : {dial_state::WaterType::kStill,
                                     dial_state::WaterType::kMedium,
                                     dial_state::WaterType::kSparkling}) {
    if (std::strcmp(text, dial_state::WaterTypeLabel(type)) == 0) {
      *out = type;
      return true;
    }
  }
  return false;
}

// Sends a JSON body with a custom status line -- httpd_resp_send_err()
// only covers a fixed set of codes (400/401/404/500/...), none of which
// fit "a command is already in flight" (409) or "the request body is
// well-formed JSON but semantically invalid" (422) precisely; this is
// the documented way to send any other status with esp_http_server (see
// httpd_resp_set_status()'s own header comment).
void SendJsonStatus(httpd_req_t* req, const char* status_line,
                    const char* json_body) {
  httpd_resp_set_status(req, status_line);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_body, HTTPD_RESP_USE_STRLEN);
}

// Shared by HandleApiDispensePost()/HandleApiStopPost(): maps a
// dial_api::RequestResult to the exact status line/body this API's own
// documented contract (docs/m15_ha_integration.md §3.2) promises.
void SendRequestResult(httpd_req_t* req, dial_api::RequestResult result) {
  switch (result) {
    case dial_api::RequestResult::kAccepted:
      SendJsonStatus(req, "200 OK", "{\"status\":\"accepted\"}");
      return;
    case dial_api::RequestResult::kRejectedNotAvailable:
      SendJsonStatus(req, "409 Conflict",
                     "{\"status\":\"rejected\",\"reason\":\"not_available\"}");
      return;
    case dial_api::RequestResult::kRejectedNotReady:
      SendJsonStatus(req, "503 Service Unavailable",
                     "{\"status\":\"rejected\",\"reason\":\"ble_not_ready\"}");
      return;
    case dial_api::RequestResult::kTimeout:
      ESP_LOGE(kTag, "API command timed out waiting for the app task -- "
               "this should not happen at the normal 20ms loop cadence");
      SendJsonStatus(req, "500 Internal Server Error",
                     "{\"status\":\"error\",\"reason\":\"timeout\"}");
      return;
  }
}
}  // namespace

ProvisioningServer::ProvisioningServer(
    time_service::WifiConnection& wifi_connection,
    const ProvisioningSecretProvider& secret_provider,
    grohe_ble::NvsCredentialsProvider& credentials_provider,
    const ApiSecretProvider& api_secret_provider,
    dial_api::DialApiHandler& dial_api)
    : wifi_connection_(wifi_connection),
      secret_provider_(secret_provider),
      credentials_provider_(credentials_provider),
      api_secret_provider_(api_secret_provider),
      dial_api_(dial_api) {}

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
  const bool provisioning_enabled = secret_provider_.Get() != nullptr &&
                                    secret_provider_.Get()[0] != '\0';
  const bool api_enabled = api_secret_provider_.Get() != nullptr &&
                           api_secret_provider_.Get()[0] != '\0';
  if (!provisioning_enabled) {
    ESP_LOGW(kTag,
             "No provisioning secret configured "
             "(provisioning_secret_local.hpp) -- /provision disabled; "
             "the existing local dev credentials are unaffected");
  }
  if (!api_enabled) {
    ESP_LOGW(kTag,
             "No local-API secret configured (api_secret_local.hpp) -- "
             "/api/* disabled; the physical dial is otherwise unaffected");
  }
  if (!provisioning_enabled && !api_enabled) {
    ESP_LOGW(kTag, "Nothing enabled -- the HTTP server itself will not start");
    return;
  }

  // A second permanent consumer of WifiConnection alongside ota::OtaServer
  // -- this reference is held for the whole process lifetime (Release()
  // is never called), matching this endpoint's own "permanently
  // reachable whenever Wi-Fi is connected, no button/gesture gating"
  // requirement. Non-blocking: app startup (display, encoder, BLE)
  // proceeds immediately regardless of how long Wi-Fi takes to associate,
  // or whether it ever does -- neither /provision nor /api/* is a
  // runtime dependency, the same principle as every other WifiConnection
  // consumer.
  wifi_connection_.AcquireAsync(
      [this]() { StartServer(); },
      [this]() {
        ESP_LOGW(kTag,
                 "Wi-Fi did not connect -- the local HTTP server is "
                 "unavailable this boot; USB/local credential fallback is "
                 "unaffected");
      });
}

void ProvisioningServer::StartServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = kProvisioningPort;
  // Hardware-found (M13.2): HTTPD_DEFAULT_CONFIG()'s own ctrl_port default
  // (ESP_HTTPD_DEF_CTRL_PORT, 32768) is a fixed loopback UDP port every
  // esp_http_server instance uses for its own internal task signaling,
  // independent of server_port. ota::OtaServer's instance (also
  // HTTPD_DEFAULT_CONFIG()-based, port 80) already claims that same
  // default, started earlier in App::Run() -- left unset here, this
  // instance's own httpd_start() would bind server_port 8080 successfully
  // and then immediately fail (and roll that bind back) trying to create
  // its control socket on the now-already-taken 32768, so nothing would
  // ever actually end up listening on 8080. Reusing kProvisioningPort
  // itself is safe: ctrl_port and server_port are unrelated namespaces
  // (loopback UDP vs. any-interface TCP), so there's no self-collision.
  config.ctrl_port = kProvisioningPort;
  // M15: up from 1 -- this instance now serves /provision plus the five
  // /api/* endpoints (only the ones whose own secret is actually
  // configured get registered below, but this ceiling covers all six).
  config.max_uri_handlers = 6;
  config.max_open_sockets = 2;
  config.lru_purge_enable = true;

  // TEMPORARY DIAGNOSTIC (whole-system RAM investigation): heap state
  // immediately before this instance's own httpd task-creation attempt
  // -- logged unconditionally, whether or not httpd_start() below then
  // succeeds.
  mem_diag::Log(kTag, "PROVISIONING_INIT");

  const esp_err_t err = httpd_start(&server_, &config);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "httpd_start failed: %s", esp_err_to_name(err));
    server_ = nullptr;
    return;
  }

  if (secret_provider_.Get() != nullptr && secret_provider_.Get()[0] != '\0') {
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
             static_cast<unsigned>(kProvisioningPort), kProvisionAuthHeader);
  }

  if (api_secret_provider_.Get() != nullptr &&
      api_secret_provider_.Get()[0] != '\0') {
    static const httpd_uri_t kApiStatusUri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = &ApiStatusGetTrampoline,
        .user_ctx = this,
    };
    static const httpd_uri_t kApiConfigGetUri = {
        .uri = "/api/config",
        .method = HTTP_GET,
        .handler = &ApiConfigGetTrampoline,
        .user_ctx = this,
    };
    static const httpd_uri_t kApiConfigPostUri = {
        .uri = "/api/config",
        .method = HTTP_POST,
        .handler = &ApiConfigPostTrampoline,
        .user_ctx = this,
    };
    static const httpd_uri_t kApiDispenseUri = {
        .uri = "/api/dispense",
        .method = HTTP_POST,
        .handler = &ApiDispensePostTrampoline,
        .user_ctx = this,
    };
    static const httpd_uri_t kApiStopUri = {
        .uri = "/api/stop",
        .method = HTTP_POST,
        .handler = &ApiStopPostTrampoline,
        .user_ctx = this,
    };
    httpd_register_uri_handler(server_, &kApiStatusUri);
    httpd_register_uri_handler(server_, &kApiConfigGetUri);
    httpd_register_uri_handler(server_, &kApiConfigPostUri);
    httpd_register_uri_handler(server_, &kApiDispenseUri);
    httpd_register_uri_handler(server_, &kApiStopUri);
    ESP_LOGI(kTag,
             "Local API ready on port %u: GET /api/status, GET/POST "
             "/api/config, POST /api/dispense, POST /api/stop (%s header "
             "required) -- see the Wi-Fi connection log above for this "
             "device's IP",
             static_cast<unsigned>(kProvisioningPort), kApiAuthHeader);
  }
}

bool ProvisioningServer::IsProvisioningAuthorized(httpd_req_t* req) const {
  const char* expected = secret_provider_.Get();
  // Init() already refused to register /provision at all if this is
  // empty -- defensive, not reachable in practice.
  if (expected == nullptr || expected[0] == '\0') {
    return false;
  }

  char received[kMaxTokenLen] = {};
  const esp_err_t err = httpd_req_get_hdr_value_str(
      req, kProvisionAuthHeader, received, sizeof(received));
  if (err != ESP_OK) {
    // ESP_ERR_NOT_FOUND (header missing) or ESP_ERR_HTTPD_RESULT_TRUNC
    // (value longer than any real token this project uses) alike -- both
    // are simply "not authorized", never logged with the value.
    return false;
  }

  return ConstantTimeEquals(received, expected);
}

bool ProvisioningServer::IsApiAuthorized(httpd_req_t* req) const {
  const char* expected = api_secret_provider_.Get();
  // Init() already refused to register any /api/* handler at all if this
  // is empty -- defensive, not reachable in practice.
  if (expected == nullptr || expected[0] == '\0') {
    return false;
  }

  char received[kMaxTokenLen] = {};
  const esp_err_t err =
      httpd_req_get_hdr_value_str(req, kApiAuthHeader, received, sizeof(received));
  if (err != ESP_OK) {
    return false;
  }

  return ConstantTimeEquals(received, expected);
}

esp_err_t ProvisioningServer::HandleProvisionPost(httpd_req_t* req) {
  // Authenticate before reading or parsing the body at all -- matches the
  // flow this mirrors in provisioning_server.hpp's own comment and
  // ota_server.cpp's identical ordering for /ota.
  if (!IsProvisioningAuthorized(req)) {
    ESP_LOGW(kTag, "Provisioning request rejected: missing or invalid %s "
             "header", kProvisionAuthHeader);
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

esp_err_t ProvisioningServer::HandleApiStatusGet(httpd_req_t* req) {
  if (!IsApiAuthorized(req)) {
    ESP_LOGW(kTag, "API request rejected: missing or invalid %s header",
             kApiAuthHeader);
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    return ESP_FAIL;
  }

  // A snapshot copy, not a live reference -- see dial_api::DialApiHandler
  // ::Status()'s own comment for why. dial_state::DialState remains the
  // single source of truth this is read from; nothing here duplicates it.
  const dial_state::DialState status = dial_api_.Status();

  cJSON* root = cJSON_CreateObject();
  cJSON_AddItemToObject(root, "connection_status",
                        cJSON_CreateString(ConnectionStatusJson(status.connection_status)));
  cJSON_AddItemToObject(root, "time_status",
                        cJSON_CreateString(TimeStatusJson(status.time_status)));
  cJSON_AddItemToObject(root, "dispense_status",
                        cJSON_CreateString(DispenseStatusJson(status.dispense_status)));
  cJSON_AddItemToObject(root, "water_type",
                        cJSON_CreateString(dial_state::WaterTypeLabel(status.water_type)));
  cJSON_AddItemToObject(root, "amount_ml", cJSON_CreateNumber(status.amount_ml));
  cJSON_AddItemToObject(root, "active_dispense_amount_ml",
                        cJSON_CreateNumber(status.active_dispense_amount_ml));
  cJSON_AddItemToObject(root, "delivered_ml", cJSON_CreateNumber(status.delivered_ml));

  cJSON* appliance_response = cJSON_CreateObject();
  cJSON_AddItemToObject(appliance_response, "received",
                        cJSON_CreateBool(status.appliance_response_received));
  cJSON_AddItemToObject(appliance_response, "success",
                        cJSON_CreateBool(status.appliance_response_success));
  cJSON_AddItemToObject(appliance_response, "code",
                        cJSON_CreateNumber(status.appliance_response_code));
  cJSON_AddItemToObject(root, "appliance_response", appliance_response);

  char* printed = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (printed == nullptr) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                         "Failed to serialize status");
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, printed, HTTPD_RESP_USE_STRLEN);
  cJSON_free(printed);
  return ESP_OK;
}

esp_err_t ProvisioningServer::HandleApiConfigGet(httpd_req_t* req) {
  if (!IsApiAuthorized(req)) {
    ESP_LOGW(kTag, "API request rejected: missing or invalid %s header",
             kApiAuthHeader);
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    return ESP_FAIL;
  }

  const settings::DialSettings config = dial_api_.Config();

  cJSON* root = cJSON_CreateObject();
  cJSON_AddItemToObject(root, "default_amount_ml",
                        cJSON_CreateNumber(config.default_amount_ml));
  cJSON_AddItemToObject(root, "amount_step_ml", cJSON_CreateNumber(config.amount_step_ml));
  cJSON_AddItemToObject(
      root, "default_water_type",
      cJSON_CreateString(dial_state::WaterTypeLabel(config.default_water_type)));

  char* printed = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (printed == nullptr) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                         "Failed to serialize config");
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, printed, HTTPD_RESP_USE_STRLEN);
  cJSON_free(printed);
  return ESP_OK;
}

esp_err_t ProvisioningServer::HandleApiConfigPost(httpd_req_t* req) {
  if (!IsApiAuthorized(req)) {
    ESP_LOGW(kTag, "API request rejected: missing or invalid %s header",
             kApiAuthHeader);
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    return ESP_FAIL;
  }

  if (req->content_len == 0 || req->content_len > kMaxBodyLen) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body size");
    return ESP_FAIL;
  }

  char body[kMaxBodyLen + 1];
  size_t received_total = 0;
  while (received_total < req->content_len) {
    const int received = httpd_req_recv(req, body + received_total,
                                        req->content_len - received_total);
    if (received == HTTPD_SOCK_ERR_TIMEOUT) {
      continue;
    }
    if (received <= 0) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Read failed");
      return ESP_FAIL;
    }
    received_total += static_cast<size_t>(received);
  }
  body[received_total] = '\0';

  cJSON* json = cJSON_ParseWithLength(body, received_total);
  if (json == nullptr) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Malformed JSON");
    return ESP_FAIL;
  }

  // Starts from the *current* persisted config, not a default-constructed
  // settings::DialSettings{} -- a request that only sets one field (e.g.
  // {"amount_step_ml": 50}) must leave the other two untouched, not reset
  // them to their compile-time defaults.
  settings::DialSettings new_values = dial_api_.Config();
  bool any_field_present = false;

  const cJSON* amount_item = cJSON_GetObjectItemCaseSensitive(json, "default_amount_ml");
  if (amount_item != nullptr) {
    any_field_present = true;
    if (!cJSON_IsNumber(amount_item) ||
        amount_item->valueint < dial_state::kMinAmountMl ||
        amount_item->valueint > dial_state::kMaxAmountMl) {
      cJSON_Delete(json);
      SendJsonStatus(req, "422 Unprocessable Entity",
                     "{\"status\":\"rejected\",\"reason\":\"invalid_default_amount_ml\"}");
      return ESP_FAIL;
    }
    new_values.default_amount_ml = amount_item->valueint;
  }

  const cJSON* step_item = cJSON_GetObjectItemCaseSensitive(json, "amount_step_ml");
  if (step_item != nullptr) {
    any_field_present = true;
    if (!cJSON_IsNumber(step_item) || step_item->valueint <= 0) {
      cJSON_Delete(json);
      SendJsonStatus(req, "422 Unprocessable Entity",
                     "{\"status\":\"rejected\",\"reason\":\"invalid_amount_step_ml\"}");
      return ESP_FAIL;
    }
    new_values.amount_step_ml = step_item->valueint;
  }

  const cJSON* water_type_item =
      cJSON_GetObjectItemCaseSensitive(json, "default_water_type");
  if (water_type_item != nullptr) {
    any_field_present = true;
    dial_state::WaterType parsed{};
    if (!cJSON_IsString(water_type_item) || water_type_item->valuestring == nullptr ||
        !ParseWaterType(water_type_item->valuestring, &parsed)) {
      cJSON_Delete(json);
      SendJsonStatus(req, "422 Unprocessable Entity",
                     "{\"status\":\"rejected\",\"reason\":\"invalid_default_water_type\"}");
      return ESP_FAIL;
    }
    new_values.default_water_type = parsed;
  }
  cJSON_Delete(json);

  if (!any_field_present) {
    SendJsonStatus(req, "422 Unprocessable Entity",
                   "{\"status\":\"rejected\",\"reason\":\"no_recognized_field\"}");
    return ESP_FAIL;
  }

  // settings::DialSettingsStore::Set() (reached through dial_api_) validates
  // again on its own (defense in depth, same reasoning as
  // HandleProvisionPost() above) and persists the whole struct as one
  // atomic NVS blob.
  if (!dial_api_.SetConfig(new_values)) {
    ESP_LOGE(kTag, "API config update failed: writing settings to NVS failed");
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                         "Failed to persist settings");
    return ESP_FAIL;
  }

  ESP_LOGI(kTag, "Dial settings updated via API");
  SendJsonStatus(req, "200 OK", "{\"status\":\"ok\"}");
  return ESP_OK;
}

esp_err_t ProvisioningServer::HandleApiDispensePost(httpd_req_t* req) {
  if (!IsApiAuthorized(req)) {
    ESP_LOGW(kTag, "API request rejected: missing or invalid %s header",
             kApiAuthHeader);
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    return ESP_FAIL;
  }

  if (req->content_len == 0 || req->content_len > kMaxBodyLen) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body size");
    return ESP_FAIL;
  }

  char body[kMaxBodyLen + 1];
  size_t received_total = 0;
  while (received_total < req->content_len) {
    const int received = httpd_req_recv(req, body + received_total,
                                        req->content_len - received_total);
    if (received == HTTPD_SOCK_ERR_TIMEOUT) {
      continue;
    }
    if (received <= 0) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Read failed");
      return ESP_FAIL;
    }
    received_total += static_cast<size_t>(received);
  }
  body[received_total] = '\0';

  cJSON* json = cJSON_ParseWithLength(body, received_total);
  if (json == nullptr) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Malformed JSON");
    return ESP_FAIL;
  }

  const cJSON* amount_item = cJSON_GetObjectItemCaseSensitive(json, "amount_ml");
  const cJSON* water_type_item = cJSON_GetObjectItemCaseSensitive(json, "water_type");

  if (!cJSON_IsNumber(amount_item) || amount_item->valueint < dial_state::kMinAmountMl ||
      amount_item->valueint > dial_state::kMaxAmountMl) {
    cJSON_Delete(json);
    SendJsonStatus(req, "422 Unprocessable Entity",
                   "{\"status\":\"rejected\",\"reason\":\"invalid_amount\"}");
    return ESP_FAIL;
  }

  dial_state::WaterType water_type{};
  if (!cJSON_IsString(water_type_item) || water_type_item->valuestring == nullptr ||
      !ParseWaterType(water_type_item->valuestring, &water_type)) {
    cJSON_Delete(json);
    SendJsonStatus(req, "422 Unprocessable Entity",
                   "{\"status\":\"rejected\",\"reason\":\"invalid_water_type\"}");
    return ESP_FAIL;
  }

  const int amount_ml = amount_item->valueint;
  cJSON_Delete(json);

  // Cross-task hand-off to the app task -- see dial_api::DialApiHandler's
  // own comment and app.cpp's RequestDispense() implementation. Never
  // touches DialController/GroheClient directly from this httpd task.
  const dial_api::RequestResult result = dial_api_.RequestDispense(amount_ml, water_type);
  ESP_LOGI(kTag, "API dispense request: %d ml, result=%d", amount_ml,
           static_cast<int>(result));
  SendRequestResult(req, result);
  return result == dial_api::RequestResult::kAccepted ? ESP_OK : ESP_FAIL;
}

esp_err_t ProvisioningServer::HandleApiStopPost(httpd_req_t* req) {
  if (!IsApiAuthorized(req)) {
    ESP_LOGW(kTag, "API request rejected: missing or invalid %s header",
             kApiAuthHeader);
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    return ESP_FAIL;
  }

  // No body -- unlike /api/dispense, stop takes no parameters.
  const dial_api::RequestResult result = dial_api_.RequestStop();
  ESP_LOGI(kTag, "API stop request: result=%d", static_cast<int>(result));
  SendRequestResult(req, result);
  return result == dial_api::RequestResult::kAccepted ? ESP_OK : ESP_FAIL;
}

}  // namespace provisioning
