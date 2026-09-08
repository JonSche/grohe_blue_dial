#include "ota/ota_server.hpp"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "firmware_info/firmware_info.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace ota {
namespace {
constexpr char kTag[] = "ota";
constexpr char kAuthHeader[] = "X-OTA-Token";
// Generous upper bound for a header value -- large enough for any
// reasonable shared secret, small enough to stay a trivial stack buffer.
constexpr size_t kMaxTokenLen = 128;

// Simple constant-time comparison. This is a local-network shared secret,
// not defense against a nation-state adversary (see ota_server.hpp's own
// comment) -- but there's no reason to leak timing information for free
// when avoiding it costs nothing. Compares full length (including a
// length mismatch) without ever short-circuiting early.
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

// Bridges esp_http_server's plain-function-pointer httpd_uri_t::handler
// to an OtaServer instance via req->user_ctx -- the same trampoline shape
// this codebase already uses for LVGL's flush callback (see
// FlushCbTrampoline, components/display/gc9a01_display.cpp).
esp_err_t VersionGetTrampoline(httpd_req_t* req) {
  return static_cast<OtaServer*>(req->user_ctx)->HandleVersionGet(req);
}

esp_err_t OtaPostTrampoline(httpd_req_t* req) {
  return static_cast<OtaServer*>(req->user_ctx)->HandleOtaPost(req);
}
}  // namespace

OtaServer::OtaServer(time_service::WifiConnection& wifi_connection,
                      const OtaSecretProvider& secret_provider)
    : wifi_connection_(wifi_connection), secret_provider_(secret_provider) {}

OtaServer::~OtaServer() {
  if (server_ != nullptr) {
    httpd_stop(server_);
  }
  // Deliberately never calls wifi_connection_.Release(): Init() (below)
  // acquires one reference for the entire process lifetime, and this
  // object is never destroyed before process exit in practice -- App
  // owns it for the lifetime of app_main(), which never returns (see
  // app.hpp).
}

void OtaServer::Init() {
  const char* secret = secret_provider_.Get();
  if (secret == nullptr || secret[0] == '\0') {
    ESP_LOGW(kTag,
             "No OTA secret configured (ota_secret_local.hpp) -- Wi-Fi OTA "
             "endpoint disabled; USB flashing is unaffected");
    return;
  }

  // A second, *permanent* consumer of WifiConnection -- unlike
  // SntpTimeProvider's one-shot Acquire()/Release() at boot, this
  // reference is held for the whole process lifetime (Release() is never
  // called) so the OTA endpoint stays reachable whenever the dial is
  // powered, not just for a brief window after boot. wifi_connection.hpp's
  // own comment already anticipated exactly this second consumer.
  // Non-blocking: app startup (display, encoder, BLE) proceeds
  // immediately regardless of how long Wi-Fi takes to associate, or
  // whether it ever does -- OTA is not a runtime dependency, the same
  // principle as every other WifiConnection consumer.
  wifi_connection_.AcquireAsync(
      [this]() { StartServer(); },
      [this]() {
        ESP_LOGW(kTag,
                 "Wi-Fi did not connect -- Wi-Fi OTA endpoint unavailable "
                 "this boot; USB flashing is unaffected");
      });
}

void OtaServer::StartServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_uri_handlers = 2;
  config.max_open_sockets = 2;
  config.lru_purge_enable = true;
  // A bit more than the 4096-byte default: esp_ota_write()'s own flash
  // write path needs some of that headroom on top of this component's
  // own recv-then-write loop (HandleOtaPost()).
  config.stack_size = 6144;

  const esp_err_t err = httpd_start(&server_, &config);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "httpd_start failed: %s", esp_err_to_name(err));
    server_ = nullptr;
    return;
  }

  const httpd_uri_t version_uri = {
      .uri = "/version",
      .method = HTTP_GET,
      .handler = &VersionGetTrampoline,
      .user_ctx = this,
  };
  const httpd_uri_t ota_uri = {
      .uri = "/ota",
      .method = HTTP_POST,
      .handler = &OtaPostTrampoline,
      .user_ctx = this,
  };
  httpd_register_uri_handler(server_, &version_uri);
  httpd_register_uri_handler(server_, &ota_uri);

  ESP_LOGI(kTag,
           "OTA endpoint ready: POST /ota (%s header required), GET "
           "/version -- see the Wi-Fi connection log above for this "
           "device's IP",
           kAuthHeader);
}

bool OtaServer::IsAuthorized(httpd_req_t* req) const {
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
    // (value longer than any real token this project uses) alike --
    // both are simply "not authorized", never logged with the value.
    return false;
  }

  return ConstantTimeEquals(received, expected);
}

esp_err_t OtaServer::HandleVersionGet(httpd_req_t* req) {
  char body[160];
  const int len = std::snprintf(
      body, sizeof(body), "version=%s\ncommit=%s\nbranch=%s%s\n",
      firmware_info::Version(), firmware_info::GitCommit(),
      firmware_info::GitBranch(), firmware_info::GitDirty() ? " (dirty)" : "");
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_send(req, body, len > 0 ? static_cast<size_t>(len) : 0);
  return ESP_OK;
}

esp_err_t OtaServer::HandleOtaPost(httpd_req_t* req) {
  // Authenticate before touching flash or reading the body at all -- see
  // the flow this mirrors in ota_server.hpp's own comment and
  // docs/ARCHITECTURE.md's "OTA" section.
  if (!IsAuthorized(req)) {
    ESP_LOGW(kTag, "OTA request rejected: missing or invalid %s header",
             kAuthHeader);
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    return ESP_FAIL;
  }

  if (req->content_len == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
    return ESP_FAIL;
  }

  const esp_partition_t* update_partition =
      esp_ota_get_next_update_partition(nullptr);
  if (update_partition == nullptr) {
    ESP_LOGE(kTag, "OTA: no inactive OTA partition available");
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                         "No OTA partition");
    return ESP_FAIL;
  }

  ESP_LOGI(kTag, "OTA: receiving %u bytes into %s",
           static_cast<unsigned>(req->content_len), update_partition->label);

  esp_ota_handle_t ota_handle = 0;
  // Exact size, not OTA_SIZE_UNKNOWN: esp_ota_begin() then only erases
  // the flash region this image actually needs, not the whole partition.
  esp_err_t err = esp_ota_begin(update_partition, req->content_len, &ota_handle);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "OTA: esp_ota_begin failed: %s", esp_err_to_name(err));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                         "esp_ota_begin failed");
    return ESP_FAIL;
  }

  // Streamed straight from the socket into flash, kRecvChunkSize bytes at
  // a time -- never buffers the whole image (see recv_buffer_'s own
  // comment in ota_server.hpp).
  size_t remaining = req->content_len;
  while (remaining > 0) {
    const size_t to_read =
        remaining < kRecvChunkSize ? remaining : kRecvChunkSize;
    const int received = httpd_req_recv(
        req, reinterpret_cast<char*>(recv_buffer_), to_read);
    if (received == HTTPD_SOCK_ERR_TIMEOUT) {
      continue;  // Retry -- matches ESP-IDF's own httpd_req_recv() examples.
    }
    if (received <= 0) {
      ESP_LOGE(kTag, "OTA: httpd_req_recv failed (%d)", received);
      esp_ota_abort(ota_handle);
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Read failed");
      return ESP_FAIL;
    }
    err = esp_ota_write(ota_handle, recv_buffer_, static_cast<size_t>(received));
    if (err != ESP_OK) {
      ESP_LOGE(kTag, "OTA: esp_ota_write failed: %s", esp_err_to_name(err));
      esp_ota_abort(ota_handle);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                           "esp_ota_write failed");
      return ESP_FAIL;
    }
    remaining -= static_cast<size_t>(received);
  }

  // Validates the image (app descriptor magic byte, chip ID, CRC) --
  // before this ever touches the boot partition selection.
  err = esp_ota_end(ota_handle);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "OTA: esp_ota_end failed: %s", esp_err_to_name(err));
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                         "Image validation failed");
    return ESP_FAIL;
  }

  err = esp_ota_set_boot_partition(update_partition);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "OTA: esp_ota_set_boot_partition failed: %s",
             esp_err_to_name(err));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                         "esp_ota_set_boot_partition failed");
    return ESP_FAIL;
  }

  ESP_LOGI(kTag, "OTA: update written and verified; rebooting into %s",
           update_partition->label);
  httpd_resp_sendstr(req, "OK\n");

  // Give the response a moment to actually leave the socket before this
  // whole device disappears -- httpd_resp_sendstr() queues the send, it
  // doesn't guarantee the peer has read it back yet.
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_restart();
  return ESP_OK;  // Unreachable -- esp_restart() never returns.
}

}  // namespace ota
