#pragma once

#include <atomic>

#include "esp_err.h"

// A self-contained, ESP-IDF-native OTA foundation (M12.4): checks for and
// installs a firmware update from a caller-supplied HTTPS URL, reusing
// esp_https_ota/esp_ota_ops entirely rather than a custom OTA protocol --
// see ota_manager.cpp and docs/ARCHITECTURE.md's own "OTA (M12.4)" section
// for the full design and the ESP-IDF idioms this mirrors.
//
// Deliberately independent of every other subsystem in this firmware: no
// knowledge of BLE, the Grohe protocol, dispensing, DialController, or UI.
// The only other component it touches is firmware_info, read-only, purely
// to know the currently running version -- see CheckForUpdate()/
// StartUpdate()'s own comments. Nothing calls CheckForUpdate()/
// StartUpdate() automatically anywhere in this milestone; both are public
// API surface for a future caller (M13's Home Assistant integration, or a
// manual trigger) to invoke deliberately -- this component has no
// background task of its own and never polls anything on its own
// initiative.
namespace ota {

enum class OtaState {
  kIdle,
  kChecking,
  kDownloading,
  kVerifying,
  kInstalling,
  kRebooting,
  kComplete,
  kFailed,
};

[[nodiscard]] constexpr const char* ToString(OtaState state) {
  switch (state) {
    case OtaState::kIdle:
      return "Idle";
    case OtaState::kChecking:
      return "Checking";
    case OtaState::kDownloading:
      return "Downloading";
    case OtaState::kVerifying:
      return "Verifying";
    case OtaState::kInstalling:
      return "Installing";
    case OtaState::kRebooting:
      return "Rebooting";
    case OtaState::kComplete:
      return "Complete";
    case OtaState::kFailed:
      return "Failed";
  }
  return "";
}

// Result of CheckForUpdate(): whether the image at the given URL
// advertises a version different from the one currently running, and
// what that version string is (meaningful only if update_available --
// matches esp_app_desc_t::version's own 32-byte size, the same struct
// firmware_info::Version() itself reads).
struct UpdateCheckResult {
  bool update_available = false;
  char remote_version[32] = {};
};

class OtaManager {
 public:
  OtaManager() = default;

  // Confirms the currently running image is healthy, cancelling any
  // pending rollback (see CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE and
  // ARCHITECTURE.md's own "Rollback" section) -- a no-op if this image
  // wasn't flashed by a pending OTA. If this boot *is* the result of an
  // unconfirmed OTA, State() reports kComplete once, until the next
  // CheckForUpdate()/StartUpdate() call moves it on. Safe to call
  // unconditionally on every boot; App::Run() calls this once, early.
  void Init();

  // Connects to `url` and reads only the new image's header -- not the
  // full image, no flash writes -- to report whether it advertises a
  // version different from firmware_info::Version() (the one currently
  // running). State() moves kIdle -> kChecking -> kIdle around the call;
  // never touches Progress().
  [[nodiscard]] UpdateCheckResult CheckForUpdate(const char* url);

  // Downloads and flashes the image at `url` into the inactive OTA
  // partition (esp_https_ota), verifies it, and -- only on success --
  // reboots into it (esp_restart(); does not return). Refuses to
  // proceed if the new image's own version exactly matches the one
  // currently running (the same defensive check ESP-IDF's own
  // advanced_https_ota example performs), reported as kFailed /
  // ESP_ERR_INVALID_VERSION. Leaves the running partition completely
  // untouched on any other failure too: esp_https_ota only ever
  // switches the boot partition inside a successful
  // esp_https_ota_finish(), never mid-download -- see the .cpp for
  // exactly which ESP-IDF call covers each failure mode this milestone
  // is required to handle (network, TLS, HTTP, invalid image,
  // verification, interrupted download).
  //
  // Blocking, synchronous, and runs entirely on the calling task -- this
  // component never spawns one of its own. Call it from whichever task
  // context is appropriate for a multi-second-to-minute HTTPS download
  // (not the LVGL or NimBLE host task).
  [[nodiscard]] esp_err_t StartUpdate(const char* url);

  [[nodiscard]] OtaState State() const { return state_.load(); }

  // 0-100 while kDownloading, or -1 if the server didn't report a
  // content length (chunked transfer -- common for a dynamically-served
  // release asset) and no honest percentage can be computed.
  [[nodiscard]] int Progress() const { return progress_.load(); }

  // The esp_err_t StartUpdate()/CheckForUpdate() itself already
  // returned on failure -- exposed here too so a caller polling State()
  // from a different task (e.g. a future Home Assistant handler) can
  // learn why without having kept the original return value.
  [[nodiscard]] esp_err_t LastError() const { return last_error_.load(); }

 private:
  // Records the failure and returns it, so every failure path in the
  // .cpp is a single `return Fail(err);` rather than three repeated
  // statements -- the one place kFailed/last_error_ are ever set.
  esp_err_t Fail(esp_err_t err);

  // Plain std::atomic, not a mutex: State()/Progress()/LastError() are
  // meant to be polled from a task other than whichever one is blocked
  // inside StartUpdate() (mirroring BleManager::conn_handle_'s and
  // SntpTimeProvider::valid_'s identical "written by a busy task, read
  // by an unrelated poller" reasoning elsewhere in this codebase) --
  // there is no compound invariant across the three fields that a
  // reader could observe torn, so independent atomics are enough.
  std::atomic<OtaState> state_{OtaState::kIdle};
  std::atomic<int> progress_{-1};
  std::atomic<esp_err_t> last_error_{ESP_OK};
};

}  // namespace ota
