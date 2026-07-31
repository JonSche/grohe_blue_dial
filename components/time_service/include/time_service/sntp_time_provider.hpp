#pragma once

#include <atomic>
#include <cstdint>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif_types.h"
#include "esp_timer.h"
#include "time_service/time_provider.hpp"
#include "time_service/wifi_credentials.hpp"

// Forward-declared: sntp_sync_time_cb_t's own parameter type
// (lwip/apps/esp_sntp.h) -- not included directly here so this header stays
// as light as the rest of this component's public surface.
struct timeval;

// SntpTimeProvider: connects to Wi-Fi once, syncs the system clock via SNTP,
// then fully tears Wi-Fi/lwIP down again -- Wi-Fi is a one-shot time source
// here, never a runtime dependency for anything else in this firmware (the
// dial keeps working, appliance control included, whether or not this ever
// succeeds). See sntp_time_provider.cpp's own top comment for the full
// event-driven design (no dedicated task, no blocking wait -- everything
// reacts to WIFI_EVENT/IP_EVENT, SNTP's own sync callback, and a one-shot
// esp_timer, all funneled through this component's own custom event base so
// every state transition still runs on a single task, exactly like
// grohe_ble::BleManager's own NimBLE-callback-driven design).
namespace time_service {

class SntpTimeProvider final : public TimeProvider {
 public:
  // wifi_credentials must outlive this object (dependency injection, not an
  // owned instance -- see wifi_credentials.hpp's own comment on why).
  explicit SntpTimeProvider(const WifiCredentialsProvider& wifi_credentials);
  ~SntpTimeProvider() override;

  SntpTimeProvider(const SntpTimeProvider&) = delete;
  SntpTimeProvider& operator=(const SntpTimeProvider&) = delete;

  // Starts the Wi-Fi connect attempt and registers every event handler this
  // class needs, then returns immediately -- this means "the attempt has
  // started," not "time is available yet." Safe to call exactly once.
  // Returns an error only if something needed to even *start* trying
  // failed (e.g. esp_wifi_init()); a Wi-Fi/SNTP failure *after* that point
  // is reported solely through IsValid()/GetCurrentEpoch() staying false,
  // never through this return value, and never crashes or blocks the rest
  // of the firmware.
  esp_err_t Init();

  [[nodiscard]] bool IsValid() const override { return valid_; }
  [[nodiscard]] bool GetCurrentEpoch(uint32_t* out_epoch_seconds) const override;

 private:
  static void OnWifiOrIpEvent(void* arg, esp_event_base_t base, int32_t id,
                              void* data);
  static void OnTimeSyncNotification(struct timeval* tv);
  static void OnSntpTimeoutTimer(void* arg);
  // The custom-event handler every real state transition funnels through
  // (see the .cpp's top comment) -- resolved via instance_, not `arg`,
  // matching this codebase's established "never trust a NimBLE-style
  // callback's own arg" discipline (see grohe_ble::BleManager).
  static void OnInternalEvent(void* arg, esp_event_base_t base, int32_t id,
                             void* data);

  void HandleWifiOrIpEvent(esp_event_base_t base, int32_t id, void* data);
  // Calls esp_wifi_connect() and tears down immediately if it fails
  // *synchronously* (e.g. an empty/invalid SSID) -- see the .cpp's own
  // comment for why this doesn't count against kMaxWifiRetries.
  void TryConnect();
  void StartSntp();
  void HandleSynced();
  void HandleSntpTimeout();
  void TearDown();

  const WifiCredentialsProvider& wifi_credentials_;
  esp_netif_t* sta_netif_ = nullptr;
  esp_timer_handle_t sntp_timeout_timer_ = nullptr;

  // All of the following are touched exclusively from OnInternalEvent()/
  // HandleWifiOrIpEvent() -- both of which the default esp_event loop
  // guarantees run serialized on its own single task -- so, like every
  // NimBLE-callback-only member in grohe_ble::BleManager, none of them need
  // their own synchronization. valid_ is the one exception: the only field
  // read from a different task (the app task, via IsValid()/
  // GetCurrentEpoch()), hence the one std::atomic here.
  int retry_count_ = 0;
  bool torn_down_ = false;

  std::atomic<bool> valid_{false};

  static SntpTimeProvider* instance_;
};

}  // namespace time_service
