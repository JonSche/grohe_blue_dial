#pragma once

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// TEMPORARY DIAGNOSTIC COMPONENT -- whole-system RAM/fragmentation
// investigation (M13.3/M14/M15's own hardware-investigation history --
// see docs/ROADMAP.md). A single, shared heap-snapshot logger every
// component in this investigation calls at its own well-defined
// state-transition points, so the entire boot's heap history ends up in
// one consistently-formatted log stream instead of N slightly different
// ad-hoc formats.
//
// Deliberately header-only: no .cpp, no linked library -- every call site
// already depends on `log` (uses ESP_LOG* itself already) and
// esp_heap_caps.h/freertos task headers are available project-wide
// already (confirmed elsewhere in this codebase: no component needs an
// explicit REQUIRES heap/freertos to use them). Consuming components add
// `mem_diag` to their own REQUIRES purely for the include path.
//
// Not a permanent addition -- remove this entire component and every
// call site once the investigation concludes.
namespace mem_diag {

// One line per checkpoint, tagged [MEM]. Reports free/largest for
// MALLOC_CAP_INTERNAL (the pool almost everything in this firmware draws
// from -- no PSRAM on this board), the INTERNAL|8BIT combination that
// FreeRTOS's own task/stack allocator actually uses
// (components/freertos/heap_idf.c's portFREERTOS_HEAP_CAPS -- see this
// investigation's own prior findings), and MALLOC_CAP_DMA (relevant for
// anything that talks to a peripheral via DMA, e.g. the SPI display) --
// three different capability views of what is nominally the same single
// internal heap on this chip, logged separately to confirm rather than
// assume they read alike.
inline void Log(const char* tag, const char* label) {
  ESP_LOGW(tag,
           "[MEM] %s: internal_free=%u internal_largest=%u "
           "internal_min_ever=%u | 8bit_free=%u 8bit_largest=%u | "
           "dma_free=%u dma_largest=%u (all bytes)",
           label,
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(
               heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(
               heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_free_size(
               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(
               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
           static_cast<unsigned>(
               heap_caps_get_largest_free_block(MALLOC_CAP_DMA)));
}

// One-shot task-stack survey: looks up every task this investigation
// cares about *by its fixed, hardcoded name* (xTaskGetHandle()) and logs
// its high-water-mark if found. Deliberately does not guess or estimate
// for a task it can't find -- logs "not found" instead, so a missing
// task is visible as missing, not silently absent from the output.
//
// Names verified against the actual ESP-IDF/NimBLE source this build
// uses (not assumed): "main" (components/freertos/app_startup.c),
// "httpd" (esp_http_server's own httpd_main.c, unconditional -- note
// this name is NOT unique: both ota::OtaServer's and provisioning::
// ProvisioningServer's httpd instances use the identical task name, so
// this lookup cannot tell them apart when both are running at once, as
// they are since M14's RAM headroom let Provisioning's own httpd_start()
// start succeeding too -- see docs/ROADMAP.md's M14 section), "nimble_host"
// (nimble_port_freertos.c, unconditional), "tiT" (lwIP's own
// TCPIP_THREAD_NAME, components/lwip/port/include/lwipopts.h). "wifi" is
// included as a well-known ESP-IDF convention but was NOT independently
// verified in this build's own source (the Wi-Fi task is created inside
// a closed-source blob) -- if it's not found, that's expected and
// documented, not a bug.
inline void LogTaskStacks(const char* tag) {
  static constexpr const char* kCandidates[] = {
      "main", "httpd", "nimble_host", "tiT", "wifi",
  };
  for (const char* name : kCandidates) {
    const TaskHandle_t handle = xTaskGetHandle(name);
    if (handle == nullptr) {
      ESP_LOGW(tag, "[MEM:TASK] \"%s\": not found", name);
      continue;
    }
    const UBaseType_t hwm_bytes = uxTaskGetStackHighWaterMark(handle);
    ESP_LOGW(tag, "[MEM:TASK] \"%s\": high_water_mark=%u bytes never touched",
             name, static_cast<unsigned>(hwm_bytes));
  }
}

}  // namespace mem_diag
