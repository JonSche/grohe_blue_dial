#pragma once

#include <atomic>
#include <functional>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "time_service/wifi_credentials.hpp"

// WifiConnection: the one place this firmware brings the Wi-Fi STA
// interface up or down. Extracted (M12.5) from what used to be
// SntpTimeProvider's own private, one-shot connect/retry/teardown state
// machine, so ota::OtaManager can share the *same* Wi-Fi session instead
// of running a second, independent connect/retry implementation against
// the one physical radio this chip has -- see
// docs/ARCHITECTURE.md#wifi-ownership-m125 for the full design and why a
// single, reference-counted, reusable connection (not two independent
// one-shot ones) is the only safe way to share it.
//
// Event-driven internally, exactly like the class this was extracted
// from: no dedicated task; everything reacts to WIFI_EVENT/IP_EVENT on
// esp_event's own default loop task. Acquire()/AcquireAsync()/Release()
// may be called from any task -- the esp_wifi_*/esp_netif_* calls they
// trigger are themselves safe to call from any task (matching this exact
// codebase's own precedent: the original SntpTimeProvider::Init() already
// called esp_wifi_init()/esp_wifi_start() directly from the app task, not
// the event loop task). This also holds for two callers racing the same
// 0->1 (first-acquirer) transition: the event-group bits for a cycle are
// only ever cleared by the *previous* cycle's Release(), never re-cleared
// at the start of the next one (see Release()'s own comment), so a
// caller that loses the race to be "first" can never observe a stale
// bit left over from an earlier cycle -- it just waits for the fresh one.
namespace time_service {

class WifiConnection {
 public:
  // wifi_credentials must outlive this object (dependency injection, not
  // an owned instance -- see wifi_credentials.hpp's own comment).
  explicit WifiConnection(const WifiCredentialsProvider& wifi_credentials);
  ~WifiConnection();

  WifiConnection(const WifiConnection&) = delete;
  WifiConnection& operator=(const WifiConnection&) = delete;

  // One-time setup (NVS, netif, the default event loop, and this class's
  // own event handlers) -- does not connect yet. Call once, from the app
  // task, before any Acquire()/AcquireAsync().
  esp_err_t Init();

  // Non-blocking: registers interest in a connection. If none is
  // currently up or being attempted, starts one (esp_netif/esp_wifi
  // bring-up, then the connect-with-retry policy this class was
  // extracted from). Otherwise joins whatever's already in flight or
  // already connected/failed. Returns immediately either way.
  //
  // on_ready()/on_failed() -- each optional, pass nullptr to skip -- run
  // on the default event loop's own task the moment *this* acquisition
  // resolves (or inline, synchronously, if it resolves immediately --
  // already connected, or a synchronous esp_wifi_*/esp_netif_* setup
  // failure). At most one pending (on_ready, on_failed) pair is tracked
  // at a time; the one caller of this non-blocking form today
  // (SntpTimeProvider) only ever calls it once, at boot -- a second,
  // concurrent AcquireAsync() call while a first is still unresolved is
  // not reachable in this codebase and its own callbacks would simply
  // not fire (Acquire()'s blocking wait below is unaffected by this,
  // since it doesn't rely on the callback mechanism at all).
  //
  // Every call -- resolved successfully or not -- holds one reference;
  // call Release() exactly once per AcquireAsync()/Acquire() call,
  // regardless of the outcome.
  void AcquireAsync(std::function<void()> on_ready,
                    std::function<void()> on_failed);

  // Blocking convenience wrapper around AcquireAsync(): blocks the
  // calling task (must not be the default event loop's own task -- that
  // would deadlock, since that's the task this class's own connection-
  // state transitions run on) until connected or given up, bounded by
  // `timeout`. Returns whether connected. Holds a reference exactly like
  // AcquireAsync() -- call Release() exactly once, regardless of the
  // result.
  [[nodiscard]] bool Acquire(TickType_t timeout);

  // Releases one Acquire()/AcquireAsync() call. Once every acquirer has
  // released, the Wi-Fi/netif driver is fully torn down (esp_wifi_stop/
  // deinit, esp_netif_destroy) -- symmetric with the bring-up
  // AcquireAsync()/Acquire() triggers, and the concrete mechanism behind
  // "must not permanently keep Wi-Fi enabled" for every caller, not just
  // OTA.
  void Release();

  [[nodiscard]] bool IsConnected() const;

 private:
  static void OnWifiOrIpEvent(void* arg, esp_event_base_t base, int32_t id,
                              void* data);
  void HandleWifiOrIpEvent(esp_event_base_t base, int32_t id, void* data);
  void TryConnect();
  esp_err_t StartConnecting();
  void Resolve(bool connected);
  void TearDownWifi();

  const WifiCredentialsProvider& wifi_credentials_;
  esp_netif_t* sta_netif_ = nullptr;
  EventGroupHandle_t events_ = nullptr;

  // Touched only from the default event loop's own task, exactly like
  // every mutable field SntpTimeProvider itself used to hold directly --
  // see HandleWifiOrIpEvent()'s own comment for why Acquire()/
  // AcquireAsync() being callable from any task doesn't change this.
  int retry_count_ = 0;
  std::function<void()> pending_ready_;
  std::function<void()> pending_failed_;

  // The one field genuinely touched from any task: Acquire()/Release()/
  // AcquireAsync() all read-modify-write it to decide "am I the first
  // one in, the last one out".
  std::atomic<int> ref_count_{0};

  static WifiConnection* instance_;
};

}  // namespace time_service
