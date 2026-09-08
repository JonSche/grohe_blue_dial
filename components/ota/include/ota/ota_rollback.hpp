#pragma once

// Bootloader rollback confirmation (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE,
// sdkconfig.defaults): a freshly-flashed OTA image boots in
// ESP_OTA_IMG_PENDING_VERIFY state, and the bootloader automatically
// reverts to the previous working image on the next boot if that state is
// still pending -- i.e. if the new image crashes, watchdogs, or loses
// power before ConfirmBootValid() below ever runs. See
// docs/ARCHITECTURE.md's "OTA" section for the full design; a USB-flashed
// image (never routed through esp_ota_ops) is entirely unaffected by any
// of this.
namespace ota {

// Confirms the currently running image is healthy -- a no-op outside a
// pending-verify boot (e.g. every normal USB-flashed boot, or the second
// and later boots of an already-confirmed OTA image). Call once, from
// App::Run(), only after every subsystem it starts up has finished
// initializing successfully -- confirming any earlier than that would
// defeat the point of rollback, since a crash during startup itself is
// exactly the failure this is meant to catch and revert.
void ConfirmBootValid();

}  // namespace ota
