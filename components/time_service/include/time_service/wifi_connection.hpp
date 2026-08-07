#pragma once

#include <atomic>
#include <functional>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif_types.h"
#include "esp_wifi_types.h"
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
// the event loop task).
//
// Stale events from our own teardown: esp_wifi_stop() (called from
// TearDownWifi(), itself called once ref_count_ drops to 0) posts
// WIFI_EVENT_STA_DISCONNECTED asynchronously as a normal side effect of
// stopping an still-associated STA -- exactly like any other WIFI_EVENT.
// That post can only be *processed* after the current call stack (up
// through Release()) returns control to the event loop's own dispatch
// loop, by which point ref_count_ may already be back at 0 with no
// acquirer holding a connection, or -- much later -- a brand new cycle
// may be in progress. Either way this event does not belong to any
// live cycle, and HandleWifiOrIpEvent()'s very first check
// (ref_count_.load() == 0) is what discards it before it can touch
// retry_count_, sta_connected_, or the event-group bits -- see that
// function's own comment. This is why kConnectedBit/kFailedBit are
// cleared at the *start* of the next Acquire()/AcquireAsync() (below),
// not at the end of the previous Release(): the ref_count_ check above
// is what actually keeps a self-inflicted stale event from poisoning
// the next cycle, not the bits' clear-timing, so there is no benefit to
// clearing any earlier, and clearing at cycle-start keeps this class's
// two "first acquirer" branches (Acquire()/AcquireAsync()) the one place
// a new cycle's state is reset, matching retry_count_/sta_connected_.
//
// This also means two callers racing the same 0->1 (first-acquirer)
// transition -- not reachable in this codebase's real usage: every
// caller (SntpTimeProvider::Init(), OtaManager's CheckForUpdate()/
// StartUpdate()) runs on the same app task, never concurrently -- could
// in theory observe a bit left over from *two* cycles back, since the
// non-first caller's xEventGroupWaitBits() doesn't wait for the first
// caller to reach the clear. Documented, not fixed, since there is no
// real caller to fix it for.
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
  // "Connected" (on_ready(), or Acquire() below returning true) always
  // means both WIFI_EVENT_STA_CONNECTED (L2 association) *and*
  // IP_EVENT_STA_GOT_IP (DHCP complete) have happened for this cycle --
  // never just the former. A caller that gets on_ready()/true has a
  // genuinely usable IP connection and needs no Wi-Fi check of its own
  // before starting HTTPS or any other networking.
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
  // Set once WIFI_EVENT_STA_CONNECTED (L2 association) fires for the
  // current cycle; IP_EVENT_STA_GOT_IP is only allowed to resolve
  // kConnectedBit while this is true -- the concrete enforcement behind
  // the "STA_CONNECTED and GOT_IP" contract documented on
  // AcquireAsync() above. Not an event-group bit of its own: nothing
  // outside this class needs to wait on association alone.
  bool sta_connected_ = false;
  // Diagnostic-only, both reset alongside retry_count_/sta_connected_ at
  // the start of each cycle: what the most recent WIFI_EVENT_STA_CONNECTED
  // this cycle reported, if any, so a later WIFI_EVENT_STA_DISCONNECTED's
  // own log can show "what we were associated with" instead of nothing --
  // the disconnected event itself carries no auth mode.
  bool bssid_established_ = false;
  wifi_auth_mode_t last_authmode_ = WIFI_AUTH_OPEN;
  std::function<void()> pending_ready_;
  std::function<void()> pending_failed_;

  // The one field genuinely touched from any task: Acquire()/Release()/
  // AcquireAsync() all read-modify-write it to decide "am I the first
  // one in, the last one out". Also read (not written) from the event
  // loop task, by HandleWifiOrIpEvent(), as the guard against our own
  // teardown's stale WIFI_EVENT_STA_DISCONNECTED -- see the top-of-file
  // comment.
  std::atomic<int> ref_count_{0};

  static WifiConnection* instance_;
};

}  // namespace time_service
