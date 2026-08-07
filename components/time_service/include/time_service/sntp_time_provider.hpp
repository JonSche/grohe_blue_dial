#pragma once

#include <atomic>
#include <cstdint>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "time_service/time_provider.hpp"
#include "time_service/wifi_connection.hpp"

// Forward-declared: sntp_sync_time_cb_t's own parameter type
// (lwip/apps/esp_sntp.h) -- not included directly here so this header stays
// as light as the rest of this component's public surface.
struct timeval;

// SntpTimeProvider: syncs the system clock via SNTP over a WifiConnection
// acquired purely as a one-shot time source, then releases it again --
// Wi-Fi is never a runtime dependency for anything else in this firmware
// (the dial keeps working, appliance control included, whether or not this
// ever succeeds). The Wi-Fi connection itself is WifiConnection's job --
// see that class's own comment; this class owns only the SNTP-specific
// sequencing on top of it.
// See sntp_time_provider.cpp's own top comment for the full event-driven
// design (no dedicated task, no blocking wait -- everything reacts to
// WifiConnection's own callback, SNTP's own sync callback, and a one-shot
// esp_timer, all funneled through this component's own custom event base so
// every state transition still runs on a single task, exactly like
// grohe_ble::BleManager's own NimBLE-callback-driven design).
namespace time_service {

class SntpTimeProvider final : public TimeProvider {
 public:
  // wifi_connection must outlive this object (dependency injection, not an
  // owned instance -- shared with whoever else might need Wi-Fi in the
  // future; see wifi_connection.hpp's own comment).
  explicit SntpTimeProvider(WifiConnection& wifi_connection);
  ~SntpTimeProvider() override;

  SntpTimeProvider(const SntpTimeProvider&) = delete;
  SntpTimeProvider& operator=(const SntpTimeProvider&) = delete;

  // Starts acquiring Wi-Fi and, once connected, SNTP sync, then returns
  // immediately -- this means "the attempt has started," not "time is
  // available yet." Safe to call exactly once. Returns an error only if
  // something needed to even *start* trying failed; a Wi-Fi/SNTP failure
  // *after* that point is reported solely through IsValid()/
  // GetCurrentEpoch() staying false, never through this return value, and
  // never crashes or blocks the rest of the firmware.
  esp_err_t Init();

  [[nodiscard]] bool IsValid() const override { return valid_; }
  [[nodiscard]] bool GetCurrentEpoch(uint32_t* out_epoch_seconds) const override;

 private:
  static void OnTimeSyncNotification(struct timeval* tv);
  static void OnSntpTimeoutTimer(void* arg);
  // The custom-event handler every real state transition funnels through
  // (see the .cpp's top comment) -- resolved via instance_, not `arg`,
  // matching this codebase's established "never trust a NimBLE-style
  // callback's own arg" discipline (see grohe_ble::BleManager).
  static void OnInternalEvent(void* arg, esp_event_base_t base, int32_t id,
                             void* data);

  // WifiConnection::AcquireAsync() callbacks -- run on the default event
  // loop's own task, exactly where HandleWifiOrIpEvent() used to run
  // before this class owned Wi-Fi directly.
  void HandleWifiReady();
  void HandleWifiFailed();

  void StartSntp();
  void HandleSynced();
  void HandleSntpTimeout();
  // SNTP-specific teardown, then releases the WifiConnection acquisition
  // Init() made -- the counterpart to AcquireAsync(), exactly one
  // Release() per Init() call.
  void TearDown();

  WifiConnection& wifi_connection_;
  esp_timer_handle_t sntp_timeout_timer_ = nullptr;

  // Touched exclusively from OnInternalEvent()/the WifiConnection
  // callbacks -- both of which run on the default event loop's own single
  // task -- so, like every callback-only member in grohe_ble::BleManager,
  // this needs no synchronization. valid_ is the one exception: the only
  // field read from a different task (the app task, via IsValid()/
  // GetCurrentEpoch()), hence the one std::atomic here.
  bool torn_down_ = false;

  std::atomic<bool> valid_{false};

  static SntpTimeProvider* instance_;
};

}  // namespace time_service
