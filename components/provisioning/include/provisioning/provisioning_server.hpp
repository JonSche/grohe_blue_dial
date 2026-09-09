#pragma once

#include "esp_http_server.h"
#include "grohe_ble/grohe_credentials.hpp"
#include "provisioning/provisioning_secret.hpp"
#include "time_service/wifi_connection.hpp"

// A minimal, local-network-only, plain-HTTP endpoint that lets Home
// Assistant (M13.4, not yet built) push this dial's Grohe BLE credentials
// once, instead of requiring a second Grohe Cloud login on the ESP32
// itself -- see ota_server.hpp and docs/ARCHITECTURE.md's "OTA (M12)"
// section for the identical transport/security shape this deliberately
// reuses (plain HTTP, no TLS, shared-secret header, constant-time
// compare, fail-closed on an empty secret), and this file's own
// "Provisioning" ARCHITECTURE.md section for why a *separate* secret and
// a *separate* esp_http_server instance/port from OTA's, not the same
// one -- components/ota/ itself is unmodified by this component.
//
// Runs its own httpd instance on kProvisioningPort (provisioning_server.cpp),
// not OTA's port 80: esp_http_server binds one instance per TCP port, and
// sharing OtaServer's own instance would mean either modifying
// components/ota/ (explicitly out of scope for this milestone) or a new
// cross-component coupling neither side needs.
//
// Deliberately independent of every other subsystem except
// time_service::WifiConnection (a second permanent consumer of it,
// alongside ota::OtaServer -- see wifi_connection.hpp's own comment,
// which already anticipated more than one) and grohe_ble (read-write, via
// the injected NvsCredentialsProvider& -- the one thing this endpoint
// actually exists to update). No knowledge of BLE transport, the Grohe
// protocol, dispensing, DialController, or UI.
//
// Not Internet-grade security: no TLS, a single shared secret compared
// with a best-effort constant-time check, no rate limiting, no replay
// protection -- identical posture to OTA's, appropriate for a trusted
// local Wi-Fi network only. See SECURITY.md and docs/ARCHITECTURE.md's
// "Provisioning" section.
namespace provisioning {

class ProvisioningServer {
 public:
  // wifi_connection/secret_provider/credentials_provider must all outlive
  // this object (dependency injection, matching ota::OtaServer's own
  // constructor pattern). credentials_provider is taken by non-const
  // reference -- unlike OtaServer's secret_provider, this endpoint's
  // entire purpose is to *mutate* it (Set()) on a valid request.
  ProvisioningServer(time_service::WifiConnection& wifi_connection,
                     const ProvisioningSecretProvider& secret_provider,
                     grohe_ble::NvsCredentialsProvider& credentials_provider);
  ~ProvisioningServer();

  ProvisioningServer(const ProvisioningServer&) = delete;
  ProvisioningServer& operator=(const ProvisioningServer&) = delete;

  // Non-blocking. If secret_provider's token is empty
  // (provisioning_secret_local.hpp was never filled in), logs that and
  // returns immediately -- the endpoint is never started unauthenticated.
  // Otherwise registers a permanent Wi-Fi acquisition (never released --
  // provisioning must stay reachable for as long as the dial has Wi-Fi at
  // all, matching this milestone's own "no button/gesture gating, no
  // temporary provisioning window" requirement) and starts the HTTP
  // server once Wi-Fi comes up. Safe to call exactly once, from
  // App::Run(), alongside wifi_connection.Init().
  void Init();

  // Public only so the free-function httpd_uri_t trampoline in
  // provisioning_server.cpp (matching ota::OtaServer's own trampoline
  // pattern) can reach it via req->user_ctx -- not meant to be called
  // from anywhere else.
  esp_err_t HandleProvisionPost(httpd_req_t* req);

 private:
  void StartServer();
  [[nodiscard]] bool IsAuthorized(httpd_req_t* req) const;

  time_service::WifiConnection& wifi_connection_;
  const ProvisioningSecretProvider& secret_provider_;
  grohe_ble::NvsCredentialsProvider& credentials_provider_;
  httpd_handle_t server_ = nullptr;
};

}  // namespace provisioning
