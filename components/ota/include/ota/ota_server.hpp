#pragma once

#include <cstdint>

#include "esp_http_server.h"
#include "ota/ota_secret.hpp"
#include "time_service/wifi_connection.hpp"

// A minimal, local-network-only, plain-HTTP OTA endpoint -- see
// ota_server.cpp and docs/ARCHITECTURE.md's "OTA" section for the full
// design, and for why this deliberately avoids esp_https_ota/TLS: M12.4's
// prior HTTPS-based OTA attempt reached hardware and failed there with a
// heap-fragmentation-driven mbedTLS allocation failure on this ESP32-C3's
// limited, PSRAM-less internal heap (Wi-Fi + BLE + LVGL all concurrently
// resident) -- see git history and ARCHITECTURE.md for the full root
// cause. Plain HTTP has no such allocation at all.
//
// Deliberately independent of every other subsystem except
// time_service::WifiConnection (a second, *permanent* consumer of it,
// alongside SntpTimeProvider's one-shot use -- see wifi_connection.hpp's
// own comment, which already anticipated exactly this) and firmware_info
// (read-only, for the /version endpoint). No knowledge of BLE, the Grohe
// protocol, dispensing, DialController, or UI.
//
// Not Internet-grade security: no TLS, a single shared secret compared
// with a best-effort constant-time check, no rate limiting, no replay
// protection. Appropriate for a trusted local Wi-Fi network only -- see
// SECURITY.md and docs/ARCHITECTURE.md's "OTA" section.
namespace ota {

class OtaServer {
 public:
  // wifi_connection/secret_provider must outlive this object (dependency
  // injection, matching grohe_ble::GroheClient/time_service::
  // WifiConnection's own constructor pattern).
  OtaServer(time_service::WifiConnection& wifi_connection,
            const OtaSecretProvider& secret_provider);
  ~OtaServer();

  OtaServer(const OtaServer&) = delete;
  OtaServer& operator=(const OtaServer&) = delete;

  // Non-blocking. If secret_provider's token is empty (ota_secret_local.hpp
  // was never filled in), logs that and returns immediately -- the OTA
  // endpoint is never started unauthenticated. Otherwise registers a
  // permanent Wi-Fi acquisition (never released -- this is the one
  // consumer in this firmware that keeps Wi-Fi connected for the whole
  // process lifetime, not just a one-shot burst) and starts the HTTP
  // server once Wi-Fi comes up. Safe to call exactly once, from
  // App::Run(), alongside wifi_connection.Init().
  void Init();

  // Public only so the free-function httpd_uri_t trampolines in
  // ota_server.cpp (matching this codebase's own FlushCbTrampoline
  // pattern in gc9a01_display.cpp) can reach them via req->user_ctx --
  // not meant to be called from anywhere else.
  esp_err_t HandleVersionGet(httpd_req_t* req);
  esp_err_t HandleOtaPost(httpd_req_t* req);

 private:
  void StartServer();
  [[nodiscard]] bool IsAuthorized(httpd_req_t* req) const;

  time_service::WifiConnection& wifi_connection_;
  const OtaSecretProvider& secret_provider_;
  httpd_handle_t server_ = nullptr;

  // Fixed-size streaming buffer for HandleOtaPost(): the firmware image
  // is never buffered in full, only kRecvChunkSize bytes at a time
  // straight into esp_ota_write() -- see that function's own comment. 4
  // KiB keeps the number of esp_ota_write()/flash calls per image
  // reasonable without ever approaching the ~16 KiB contiguous
  // allocation that doomed M12.4's HTTPS OTA (this transport has no such
  // allocation at all, but the size is kept modest anyway on an
  // ESP32-C3 with no PSRAM).
  static constexpr size_t kRecvChunkSize = 4096;
  uint8_t recv_buffer_[kRecvChunkSize];
};

}  // namespace ota
