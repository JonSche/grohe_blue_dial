#pragma once

#include <atomic>
#include <functional>
#include <vector>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif_types.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "time_service/wifi_credentials.hpp"

// WifiConnection: the one place this firmware brings the Wi-Fi STA
// interface up or down. Extracted from what used to be SntpTimeProvider's
// own private, one-shot connect/retry/teardown state machine into a
// standalone, reference-counted class so any consumer needing Wi-Fi can
// share the *same* session instead of running a second, independent
// connect/retry implementation against the one physical radio this chip
// has -- see docs/ARCHITECTURE.md#wifi-connectivity for the full design.
// Two consumers today: SntpTimeProvider's one-shot boot-time burst and
// ota::OtaServer's permanent acquisition (see ota_server.hpp) -- both are
// genuine AcquireAsync() callers of a single shared connection attempt,
// which is exactly the case AcquireAsync()'s own comment below documents.
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
// transition -- today's two real AcquireAsync() callers (OtaServer::
// Init(), SntpTimeProvider::Init(), both via App::Run()) only ever run
// sequentially on the same app task, never concurrently with each other,
// so this specific race remains unreached in practice -- could in theory
// observe a bit left over from *two* cycles back, since a later caller's
// own bit check (AcquireAsync()'s "already resolved?" check, or
// Acquire()'s xEventGroupWaitBits()) doesn't wait for the first caller to
// reach the clear. Documented, not fixed, since there is still no real
// caller landing concurrently with a fresh first-acquirer to fix it for
// -- unlike the "second caller arrives before this cycle resolves" case
// (see AcquireAsync() below), which two real, sequential-but-both-async
// callers made genuinely reachable, and which pending_waiters_/
// waiters_mutex_ below fix directly.
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
  // resolves (or inline, synchronously, on the calling task, if it
  // resolves immediately -- already connected, already failed, or a
  // synchronous esp_wifi_*/esp_netif_* setup failure).
  //
  // Multiple concurrent callers are fully supported -- an arbitrary
  // number of pending (on_ready, on_failed) pairs are tracked at once
  // (pending_waiters_ below), not just one. A caller arriving while a
  // connection attempt is already in flight (whether it's the first
  // caller that just started it, or a later one) is queued and notified
  // -- exactly once -- when that attempt resolves, same as every other
  // pending caller; a caller arriving after this cycle already resolved
  // is told immediately, synchronously, on the calling task. Safe to
  // call from multiple different tasks concurrently -- see
  // waiters_mutex_'s own comment for the synchronization this relies on.
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
  // "Wi-Fi is never a runtime dependency" for every caller.
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

  // One entry per in-flight AcquireAsync() call still waiting on this
  // cycle's resolution -- replaces what used to be a single (on_ready,
  // on_failed) pair, which silently dropped every caller after the
  // first if a second one arrived before the connection resolved (the
  // M12 bug this fixes: ota::OtaServer::Init() and
  // SntpTimeProvider::Init() both now call AcquireAsync() during the
  // same boot, and Wi-Fi association+DHCP reliably outlasts the display/
  // encoder init that runs between them).
  struct AsyncWaiter {
    std::function<void()> on_ready;
    std::function<void()> on_failed;
  };
  std::vector<AsyncWaiter> pending_waiters_;

  // Guards pending_waiters_, and jointly with it, the "has this cycle
  // already resolved?" check against events_'s own bits (see
  // AcquireAsync()/Resolve() in the .cpp). AcquireAsync() can run on any
  // calling task, while Resolve() always runs on the default event
  // loop's own task, so a caller's "check whether we're already
  // resolved, else queue" and Resolve()'s "mark resolved, then drain
  // every queued waiter" must be atomic with respect to each other --
  // without this lock serializing the two, a waiter could be queued
  // just after a drain already ran for this cycle (stranded until some
  // *later*, unrelated cycle resolves) if the two steps interleaved the
  // other way. Taking this same lock on both sides is what rules that
  // interleaving out entirely: whichever of a given AcquireAsync() call
  // and Resolve() takes the lock first is fully serialized before the
  // other can even read events_'s bits or pending_waiters_.
  //
  // A plain (non-recursive) mutex, not a critical section: the protected
  // region is always just a bit check plus a vector push_back/swap, and
  // is always released *before* any callback in this class ever runs --
  // see Resolve()'s own comment for why holding it across an arbitrary
  // caller-supplied callback would risk a self-deadlock (a callback that
  // itself calls AcquireAsync()/Release() synchronously, on the same
  // task already holding this lock) instead of just calling back into
  // already-free code.
  SemaphoreHandle_t waiters_mutex_ = nullptr;

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
