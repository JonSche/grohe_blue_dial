#include "time_service/sntp_time_provider.hpp"

#include "apps/esp_sntp.h"
#include "esp_log.h"

// This provider acquires Wi-Fi (via WifiConnection -- see
// wifi_connection.hpp) purely as a one-shot SNTP time source, then
// releases it again -- never a runtime dependency for anything else in
// this firmware (see the header's own comment). The design is entirely
// event-driven, with no dedicated FreeRTOS task and no
// blocking wait, mirroring how grohe_ble::BleManager itself never spins up
// a task to poll NimBLE -- it reacts to NimBLE's own callbacks. Here:
//
//   WifiConnection::AcquireAsync()'s own callbacks (HandleWifiReady()/
//   HandleWifiFailed()) -- run on the default event loop's own task,
//   exactly where this class used to handle WIFI_EVENT/IP_EVENT directly
//   before Wi-Fi connection management moved into WifiConnection (M12.5).
//
//   SNTP's own sync-notification callback (OnTimeSyncNotification) and the
//   SNTP timeout esp_timer (OnSntpTimeoutTimer) do NOT run on that same
//   task -- the former fires on lwIP's own tcpip task, the latter on
//   esp_timer's own service task. Rather than adding real synchronization
//   (a mutex) around the shared retry/teardown state, both immediately
//   re-post as a custom event (kInternalEventBase) onto the *same* default
//   event loop WifiConnection's own events run on. esp_event guarantees
//   every handler on one loop runs serialized on that loop's single task,
//   so OnInternalEvent() ends up handling synced/timeout exactly like a
//   third kind of Wi-Fi-adjacent event -- meaning every piece of mutable
//   state this class has (other than the one atomic `valid_`, read from a
//   different task entirely) is genuinely touched from just one task,
//   without needing its own lock.
namespace time_service {
namespace {
constexpr char kTag[] = "sntp_time_provider";
constexpr char kSntpServer[] = "pool.ntp.org";
constexpr uint64_t kSntpTimeoutUs = 15'000'000;  // 15 s

ESP_EVENT_DEFINE_BASE(kInternalEventBase);
enum InternalEventId {
  kInternalEventSynced,
  kInternalEventTimeout,
};

// Foreign-task callbacks (lwIP's tcpip task, esp_timer's service task) post
// with a short, bounded timeout rather than blocking indefinitely -- if the
// default event loop's own queue were ever full, dropping this one post is
// preferable to stalling someone else's task.
constexpr TickType_t kPostTimeout = pdMS_TO_TICKS(1000);
}  // namespace

SntpTimeProvider* SntpTimeProvider::instance_ = nullptr;

SntpTimeProvider::SntpTimeProvider(WifiConnection& wifi_connection)
    : wifi_connection_(wifi_connection) {
  instance_ = this;
}

SntpTimeProvider::~SntpTimeProvider() {
  if (instance_ == this) {
    instance_ = nullptr;
  }
}

esp_err_t SntpTimeProvider::Init() {
  const esp_err_t err = esp_event_handler_instance_register(
      kInternalEventBase, ESP_EVENT_ANY_ID, &OnInternalEvent, nullptr, nullptr);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "register internal event handler failed: %s",
             esp_err_to_name(err));
    return err;
  }

  wifi_connection_.AcquireAsync([this] { HandleWifiReady(); },
                                [this] { HandleWifiFailed(); });

  ESP_LOGI(kTag, "Wi-Fi/SNTP time sync started (event-driven, non-blocking)");
  return ESP_OK;
}

bool SntpTimeProvider::GetCurrentEpoch(uint32_t* out_epoch_seconds) const {
  if (!valid_) {
    return false;
  }
  const time_t now = time(nullptr);
  if (now < 0) {
    return false;
  }
  *out_epoch_seconds = static_cast<uint32_t>(now);
  return true;
}

void SntpTimeProvider::HandleWifiReady() {
  if (torn_down_) {
    return;  // The timeout already fired and gave up first; ignore.
  }
  ESP_LOGI(kTag, "Wi-Fi connected; starting SNTP sync");
  StartSntp();
}

void SntpTimeProvider::HandleWifiFailed() {
  if (torn_down_) {
    return;
  }
  ESP_LOGE(kTag, "Wi-Fi connect failed; giving up on SNTP sync");
  TearDown();
}

void SntpTimeProvider::StartSntp() {
  sntp_set_time_sync_notification_cb(&OnTimeSyncNotification);
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, kSntpServer);
  esp_sntp_init();

  const esp_timer_create_args_t timer_args = {
      .callback = &OnSntpTimeoutTimer,
      .arg = nullptr,
      .name = "sntp_timeout",
  };
  const esp_err_t err = esp_timer_create(&timer_args, &sntp_timeout_timer_);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "esp_timer_create failed: %s", esp_err_to_name(err));
    TearDown();
    return;
  }
  esp_timer_start_once(sntp_timeout_timer_, kSntpTimeoutUs);
}

void SntpTimeProvider::OnTimeSyncNotification(struct timeval* /*tv*/) {
  // Runs on lwIP's own tcpip task, not the default event loop's task --
  // re-post rather than touch any shared state directly here. See the
  // top-of-file comment for why.
  esp_event_post(kInternalEventBase, kInternalEventSynced, nullptr, 0,
                 kPostTimeout);
}

void SntpTimeProvider::OnSntpTimeoutTimer(void* /*arg*/) {
  // Runs on esp_timer's own service task -- same reasoning as
  // OnTimeSyncNotification() above.
  esp_event_post(kInternalEventBase, kInternalEventTimeout, nullptr, 0,
                 kPostTimeout);
}

void SntpTimeProvider::OnInternalEvent(void* /*arg*/, esp_event_base_t /*base*/,
                                       int32_t id, void* /*data*/) {
  auto* self = instance_;
  if (self == nullptr) {
    return;
  }
  if (id == kInternalEventSynced) {
    self->HandleSynced();
  } else if (id == kInternalEventTimeout) {
    self->HandleSntpTimeout();
  }
}

void SntpTimeProvider::HandleSynced() {
  if (torn_down_) {
    return;  // The timeout already fired and gave up first; ignore.
  }
  ESP_LOGI(kTag, "SNTP sync succeeded; system clock set");
  valid_ = true;
  TearDown();
}

void SntpTimeProvider::HandleSntpTimeout() {
  if (torn_down_) {
    return;  // Sync already succeeded first; ignore.
  }
  ESP_LOGE(kTag, "SNTP sync timed out; giving up");
  TearDown();
}

void SntpTimeProvider::TearDown() {
  if (torn_down_) {
    return;
  }
  torn_down_ = true;

  if (sntp_timeout_timer_ != nullptr) {
    esp_timer_stop(sntp_timeout_timer_);
    esp_timer_delete(sntp_timeout_timer_);
    sntp_timeout_timer_ = nullptr;
  }
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }
  wifi_connection_.Release();
  ESP_LOGI(kTag, "SNTP torn down; %s", valid_ ? "time available"
                                              : "time NOT available");
}

}  // namespace time_service
