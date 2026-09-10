#pragma once

#include "dial_api/dial_api.hpp"
#include "esp_http_server.h"
#include "grohe_ble/grohe_credentials.hpp"
#include "provisioning/api_secret.hpp"
#include "provisioning/provisioning_secret.hpp"
#include "time_service/wifi_connection.hpp"

// A minimal, local-network-only, plain-HTTP endpoint. Originally (M13.2)
// just `POST /provision`, letting Home Assistant push this dial's Grohe
// BLE credentials once instead of requiring a second Grohe Cloud login
// on the ESP32 itself. As of M15, this is also where the dial's local
// HTTP API lives (`GET /api/status`, `GET`/`POST /api/config`,
// `POST /api/dispense`, `POST /api/stop`) -- the transport the native
// Home Assistant integration talks to, replacing M13.3's now-removed
// MQTT client -- see docs/m15_ha_integration.md. Both live on the same
// esp_http_server instance/port deliberately (see kProvisioningPort's
// own comment): a second/third httpd instance would mean a second/third
// httpd task+stack for no benefit, and this endpoint's own header
// comment already anticipated exactly this consumer.
//
// See ota_server.hpp and docs/ARCHITECTURE.md's "OTA (M12)" section for
// the identical transport/security shape /provision deliberately reuses
// (plain HTTP, no TLS, shared-secret header, constant-time compare,
// fail-closed on an empty secret) -- the /api/* endpoints reuse the same
// shape again, gated by their own separate secret (api_secret.hpp, not
// provisioning_secret.hpp -- see that header's own comment for why).
//
// Deliberately independent of BLE transport/the Grohe protocol/UI: the
// /api/* handlers reach dial_controller_/grohe_client_ (owned by
// app::App, never by this class) only through the injected
// dial_api::DialApiHandler& -- see that interface's own comment for why,
// and app.cpp for the cross-task hand-off it performs so no BLE/
// DialController call ever happens on this class's own httpd task.
//
// Not Internet-grade security: no TLS, single shared secrets compared
// with a best-effort constant-time check, no rate limiting, no replay
// protection beyond what the BLE protocol's own timestamp already
// provides -- identical posture to OTA's/the original /provision
// endpoint's, appropriate for a trusted local Wi-Fi network only. See
// SECURITY.md and docs/m15_ha_integration.md's security section.
namespace provisioning {

class ProvisioningServer {
 public:
  // wifi_connection/secret_provider/credentials_provider/
  // api_secret_provider/dial_api must all outlive this object
  // (dependency injection, matching ota::OtaServer's own constructor
  // pattern). credentials_provider is taken by non-const reference --
  // unlike OtaServer's secret_provider, /provision's entire purpose is
  // to *mutate* it (Set()) on a valid request. dial_api is likewise
  // non-const: /api/dispense, /api/stop, and POST /api/config all
  // mutate dial state indirectly through it (see dial_api.hpp).
  ProvisioningServer(time_service::WifiConnection& wifi_connection,
                     const ProvisioningSecretProvider& secret_provider,
                     grohe_ble::NvsCredentialsProvider& credentials_provider,
                     const ApiSecretProvider& api_secret_provider,
                     dial_api::DialApiHandler& dial_api);
  ~ProvisioningServer();

  ProvisioningServer(const ProvisioningServer&) = delete;
  ProvisioningServer& operator=(const ProvisioningServer&) = delete;

  // Non-blocking. Registers a permanent Wi-Fi acquisition (never
  // released -- both /provision and /api/* must stay reachable for as
  // long as the dial has Wi-Fi at all) and starts the HTTP server once
  // Wi-Fi comes up. /provision is registered only if secret_provider's
  // token is non-empty; the /api/* endpoints only if
  // api_secret_provider's token is non-empty -- each independently, so
  // one can be configured without the other. If *neither* token is set,
  // the HTTP server itself never starts at all (nothing to serve).
  // Safe to call exactly once, from App::Run(), alongside
  // wifi_connection.Init().
  void Init();

  // Public only so the free-function httpd_uri_t trampolines in
  // provisioning_server.cpp (matching ota::OtaServer's own trampoline
  // pattern) can reach them via req->user_ctx -- not meant to be called
  // from anywhere else.
  esp_err_t HandleProvisionPost(httpd_req_t* req);
  esp_err_t HandleApiStatusGet(httpd_req_t* req);
  esp_err_t HandleApiConfigGet(httpd_req_t* req);
  esp_err_t HandleApiConfigPost(httpd_req_t* req);
  esp_err_t HandleApiDispensePost(httpd_req_t* req);
  esp_err_t HandleApiStopPost(httpd_req_t* req);

 private:
  void StartServer();
  [[nodiscard]] bool IsProvisioningAuthorized(httpd_req_t* req) const;
  [[nodiscard]] bool IsApiAuthorized(httpd_req_t* req) const;

  time_service::WifiConnection& wifi_connection_;
  const ProvisioningSecretProvider& secret_provider_;
  grohe_ble::NvsCredentialsProvider& credentials_provider_;
  const ApiSecretProvider& api_secret_provider_;
  dial_api::DialApiHandler& dial_api_;
  httpd_handle_t server_ = nullptr;
};

}  // namespace provisioning
