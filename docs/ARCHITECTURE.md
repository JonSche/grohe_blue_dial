# Architecture

Grohe Dial is an ESP-IDF (C++20) firmware for the VIEWE
UEDX24240013-MD50E-B: an ESP32-C3 with a 240x240 round GC9A01 SPI display and
an onboard rotary encoder + button. See [`../README.md`](../README.md) for
build instructions and the pinout table.

## Goals behind the structure

- **One component, one concern.** Each component owns exactly one piece of
  hardware or one layer of the stack, and exposes a small C++ class instead
  of loose C functions and globals.
- **Dependencies point one way.** `board` and `dial_state` sit at the
  bottom (no dependencies of their own); `display`, `encoder` and `ui` each
  depend only on `board`/`dial_state` (and, for `display`/`ui`, LVGL); `app`
  is the only component allowed to know about all of them at once. Nothing
  depends on `app`.
- **New hardware is a new sibling component**, not a change to existing
  ones. This is what let `grohe_ble` slot in without touching `display`,
  `encoder`, or `ui` — see [BLE](#ble-grohe_ble) below.

## Component map

```
components/
  board/       Pin/bus constants only (header-only). No behavior.
  dial_state/  DialState struct + WaterType enum + the amount range/step
               constants (header-only). The single source of truth for the
               dial UI; depends on nothing so both app/ and ui/ can share it
               without inverting the dependency graph.
  bringup/     ColorCycleTest: raw esp_lcd + esp_lcd_gc9a01 smoke test (no
               LVGL). Standalone diagnostic, not part of the app/ stack.
  display/     Gc9a01Display: SPI bus + a ported GC9A01 panel driver
               (lcd_panel_gc9a01.c, replacing the generic esp_lcd_gc9a01
               registry driver) + a self-owned LVGL v9 integration (no
               esp_lvgl_port). Produces an lv_display_t* and a lock/unlock
               pair.
  encoder/     RotaryEncoder (GPIO-ISR quadrature decode) and Button
               (polled, debounced by the caller) are the raw hardware
               access. EncoderInput wraps both and turns them into typed
               EncoderEvents (RotateCW/RotateCCW/ShortPress/LongPress) --
               no LVGL dependency, no knowledge of what the events mean.
  ui/          UiManager: the LVGL screen/widget tree for the dial screen
               (progress ring, amount, water type, hint). Takes an
               lv_display_t* and a DialState to render from -- no knowledge
               of SPI, GPIO, esp_lcd, or the encoder. Contains no business
               logic: Render() is a pure function of DialState.
  grohe_ble/   BleManager owns the NimBLE host stack lifecycle and the BLE
               connection state machine (Idle/Initializing/Scanning/
               Connecting/Discovering/Ready/Disconnected/Backoff -- only
               Idle->Initializing->Scanning is reachable so far; see "BLE"
               below). GroheClient is a thin facade over it -- the one
               class app/ is allowed to talk to for BLE, following the
               same "app never reaches past the facade" pattern as
               display/encoder/ui.
  app/         App: the composition root, plus DialController -- the
               "Application Controller" that owns the one DialState
               instance and applies the interaction rules (amount
               clamping, water-type toggle, dispense logging) in response
               to EncoderEvents. App itself only wires
               EncoderInput::Poll() -> DialController::HandleEvent() ->
               UiManager::Render(), and separately drains
               GroheClient::Poll() the same way; it contains no rules of
               its own.
main/          app_main() -> app::App.
```

Dependency graph (arrows = "depends on"):

```
bringup   --> board                        (standalone; no lvgl)

app --> display   --> board
app --> ui        --> dial_state, (lvgl)
app --> encoder   --> board
app --> grohe_ble --> (bt/nimble, nvs_flash)
app --> dial_state
```

`bringup` intentionally duplicates the handful of `esp_lcd`/`esp_lcd_gc9a01`
init calls that also appear in `display/gc9a01_display.cpp`, rather than
depending on `display` itself — pulling in `display` would pull in its
`lvgl` manifest dependency too (the ESP-IDF component manager resolves a
component's declared dependencies as soon as that component is part of the
build, regardless of which of its classes are actually used), which defeats
the point of a bring-up test meant to isolate panel/SPI/backlight issues
from the UI framework. If this duplication grows beyond basic panel
bring-up, it's worth extracting a shared "raw panel init" helper that both
`bringup` and `display` depend on.

`ui` never includes `display/gc9a01_display.hpp`; it only takes the
`lv_display_t*` that `display` produces. This is why `ui` and `display` can
each change independently (e.g. swapping the panel driver, or replacing the
boot screen with a real dial UI) without touching one another.

## Runtime model

- **LVGL task**: self-owned, created and torn down entirely inside
  `Gc9a01Display::Init()`/`~Gc9a01Display()` — there is no `esp_lvgl_port`
  dependency. `Init()` sequences: `lv_init()` → allocate the frame buffer →
  `lv_display_create()`/`set_buffers()`/`set_flush_cb()` → register the
  panel IO's `on_color_trans_done` callback (calls `lv_display_flush_ready()`
  from ISR context) → start a 2ms `esp_timer` driving `lv_tick_inc()` →
  create a recursive mutex → create the dedicated LVGL task. That task loops
  `Lock() → lv_timer_handler() → Unlock() → vTaskDelay(10ms)` forever.
  Nothing in this codebase talks to the panel directly outside of
  `Gc9a01Display::Init()`/`~Gc9a01Display()`.
  - **Small row-based partial buffer, not a full-screen framebuffer.**
    (M3.2) A full 240×240 RGB565 frame is 115,200 bytes; a BLE memory RCA
    established that enabling NimBLE on ESP32-C3 shrinks the DMA-capable
    heap below that, and a follow-up architecture review established that
    neither LVGL, the GC9A01 (which has its own GRAM and full CASET/RASET
    windowed-write support — see `lcd_panel_gc9a01.c`), nor this UI's small,
    localized widget updates actually require a full-frame buffer — that
    was inherited from the vendor's reference `lvgl_port.c`, not a real
    requirement. `Gc9a01Display` now uses `LV_DISPLAY_RENDER_MODE_PARTIAL`
    with a 30-row buffer (14,400 bytes, ~87% smaller). LVGL computes buffer
    height as `buf_size / stride`, so this is exactly a "30 full-width rows"
    buffer, not an arbitrary byte count. Single-buffered, as before: LVGL
    waits for this (now much smaller and faster) buffer's flush to
    complete before rendering the next chunk, which remains imperceptible
    for this mostly-static round-dial UI.
  - **Task period is 10ms, not the vendor's 5ms.** `CONFIG_FREERTOS_HZ=100`
    means one tick is 10ms; `pdMS_TO_TICKS(5)` truncates to `0` ticks under
    integer division, which made `vTaskDelay()` a no-op `taskYIELD()`
    instead of a real sleep — the LVGL task then consumed ~99.5% of the CPU
    and starved the idle task, tripping `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0`'s
    watchdog every 5 seconds. 10ms is the smallest delay at this tick rate
    that actually blocks.
- **Main task** (`app_main` / `app::App::Run()`): after bringing every
  subsystem up, this becomes a 20 ms loop that drains
  `EncoderInput::Poll()` into `DialController::HandleEvent()`, drains
  `GroheClient::Poll()` (log-only until a future milestone wires BLE
  events into `DialState` -- see below), and re-renders `UiManager` only
  when something actually changed.
- **NimBLE host task**: created by `nimble_port_freertos_init()` inside
  `BleManager::Init()`, running `nimble_port_run()` for the process
  lifetime. NimBLE's GAP/host callbacks (`ble_hs_cfg.sync_cb`/`reset_cb`)
  fire on this task, never on the main task -- they do the minimum
  possible work (update `BleManager`'s state, build a `BleEvent`, push it
  onto a small bounded `xQueueSend(..., 0)` queue) and never touch
  `DialState`/LVGL directly. `App::Run()`'s main-task loop drains that
  queue via `GroheClient::Poll()` on every iteration
  (`xQueueReceive(..., 0)`, non-blocking), the same shape as
  `EncoderInput::Poll()` -- this is the one hand-off point between the two
  tasks, and it is the only place a future milestone needs to touch to
  start driving `DialState`/`UiManager` from BLE events.
- **Locking**: LVGL itself is not thread-safe. Any code that touches LVGL
  objects from outside the LVGL task must hold the lock (a recursive mutex,
  safe against nested `Lock()` calls from the same task):

  ```cpp
  if (display::Gc9a01Display::Lock()) {
    // lv_* calls here
    display::Gc9a01Display::Unlock();
  }
  ```

  `app::App::Run()` already does this once, around `ui_.Init(...)`. Any
  future code that updates the UI from the main task (e.g. in response to a
  BLE event or an encoder turn) must follow the same pattern.

## Why GPIO-ISR (not PCNT) for the encoder

The rotary encoder is decoded in software via GPIO edge interrupts, not the
PCNT peripheral: **ESP32-C3 has no PCNT hardware at all** (`SOC_PCNT_SUPPORTED`
is unset for this target — confirmed absent from both
`soc/esp32c3/include/soc/soc_caps.h` and this project's generated
`sdkconfig`), so `esp_driver_pcnt` builds as an empty component here and any
`pcnt_*` call is an undefined reference at link time. This was originally
missed because `main.cpp` didn't link `app`/`encoder` into the binary until
well after the PCNT-based version was written — the very first real link
against this target caught it.

Both phase pins are configured `GPIO_INTR_ANYEDGE`; every edge is resolved
directly in the ISR via a standard 4×4 quadrature transition table indexed
by `(previous_state << 2) | new_state`, updating an atomic accumulator —
no queue, no dedicated decode task. `encoder::RotaryEncoder::GetPosition()`
is a relaxed atomic load.

## BLE (`grohe_ble`)

A dedicated architecture design review (see the M3 review in the project
history) considered splitting this into two components (a generic
NimBLE-wrapping `ble/` plus a Grohe-specific `grohe_ble/`), then explicitly
rejected that split: this firmware will only ever talk to one BLE
peripheral, so a generic reusable BLE layer is premature abstraction.
`components/grohe_ble/` is the single component, containing two classes:

- **`BleManager`** owns the NimBLE host stack lifecycle
  (`nimble_port_init()`/`nimble_port_freertos_init()`) and the connection
  state machine (`BleState`: `Idle`/`Initializing`/`Scanning`/`DeviceFound`/
  `Connecting`/`Connected`/`DiscoveringServices`/`ReadyForProtocol`/`Ready`/
  `Disconnected`/`Backoff`). As of M5,
  `Idle -> Initializing -> Scanning -> DeviceFound -> Connecting ->
  Connected -> DiscoveringServices -> ReadyForProtocol` is reachable end to
  end: the host starts, syncs, scans until the Grohe service UUID is seen,
  connects, negotiates MTU, and walks the full GATT hierarchy (all primary
  services, then all characteristics per service, one service at a time).
  `Ready` and `Backoff` remain unimplemented — no protocol communication or
  reconnect logic yet.
- **`ble_constants.hpp`** holds the BLE protocol constants — currently just
  the Grohe service UUID, which appears exactly once in the codebase.
  Future service and characteristic UUIDs belong here too, so the protocol
  surface stays reviewable in one place.
- **`GroheClient`** is a thin facade over `BleManager` — the one class
  `app/` is allowed to talk to for BLE, mirroring how `app/` never reaches
  past `display`/`encoder`/`ui`'s own top-level classes either. It still
  does nothing beyond forwarding `Init()`/`Poll()`; this is where GATT-level
  Grohe protocol handling (reads/writes/notifications) will live once a
  future milestone adds it.

**GATT discovery** (M5): once connected, `ble_gattc_exchange_mtu()` is
attempted (non-fatal if the peer declines — discovery still works at the
default 23-byte MTU), then `ble_gattc_disc_all_svcs()` discovers every
primary service, and `ble_gattc_disc_all_chrs()` walks each one's
characteristics in turn, sequentially — not concurrently, matching
ESP-IDF's own bundled `blecent` reference client. Only a service's handle
range is kept in memory (`BleManager::services_`, a small fixed-size array,
sized generously above this appliance's actual hierarchy) — not a GATT
cache; every UUID/handle/property is logged the instant its discovery
callback fires and never stored beyond that. A GATT-level error during
discovery actively disconnects (`ble_gap_terminate()`), matching `blecent`'s
own "discovery failed → terminate" pattern, rather than leaving a
half-discovered connection sitting on this project's single connection slot
(`CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1`).

**Two hardware-discovered robustness issues, both fixed** (found via
repeated real-device trials, not by inspection alone — the appliance's
signal is weak, around −95 to −101 dBm, close to the receiver's sensitivity
limit, which is what exposes both of these):
1. ESP-IDF's bundled NimBLE fork autonomously reattempts a connection that
   fails immediately after being established (logged as `NimBLE: Reattempt
   connection; reason = 0x3e`, gated by `MYNEWT_VAL(BLE_ENABLE_CONN_REATTEMPT)`
   in `ble_hs_hci_evt.c`). Its master-role reattempt path
   (`ble_gap_master_connect_reattempt()` in `ble_gap.c`) calls
   `ble_gap_connect()` again but passes the address of a **local stack
   variable** as `cb_arg` instead of the original, saved `cb_arg` — a
   genuine upstream bug. Trusting that pointer in `OnGapEvent` reproducibly
   corrupted state on real hardware (a logged state transition through a
   name that doesn't exist in the enum). Fixed by never trusting
   `arg`/`cb_arg` in any of `BleManager`'s NimBLE callbacks and always
   resolving the instance through `instance_` instead — the same pattern
   `OnHostSync()`/`OnHostReset()` already used, now applied uniformly.
2. Once (1) was fixed, a second, independent issue surfaced: a GATT
   procedure callback for an already-superseded connection attempt (killed
   by the same reattempt behavior) could arrive *after* a newer, good
   connection had already replaced it, and — since `conn_handle_` is a
   single shared member — would tear down the new connection based on a
   failure that actually belonged to the old one. Every GATT callback
   (`OnMtuResult`/`OnSvcDisc`/`OnChrDisc`) receives its own `conn_handle` as
   a plain integer parameter (not a pointer, so unaffected by (1)); each now
   compares it against `conn_handle_` and silently ignores the callback if
   they don't match, exactly like `HandleDiscReport()`'s existing
   idempotency check for stale advertisement reports.

**Detection strategy**: the appliance is identified solely by the 128-bit
service UUID it advertises (`kGroheServiceUuid`), matched against the
advertisement's service class UUID lists with `ble_uuid_cmp()`. This is a
direct port of the existing Python reference implementation's predicate, not
a re-derivation. It is also the only workable option in practice: the
appliance advertises **no local name** (verified on hardware — `name=''` in
every captured report), so name matching could not work, and matching on MAC
address would break for any other unit. Advertisement payloads are parsed
with NimBLE's own `ble_hs_adv_parse_fields()` rather than hand-decoded.

**Scanning** is active (`passive = 0`) and continuous (scan window == scan
interval). Controller-side duplicate filtering is deliberately **off**:
this build filters duplicates by address only
(`CONFIG_BT_CTRL_SCAN_DUPL_TYPE_DEVICE`), which would suppress a device's
scan response once its advertisement had been seen — and since a 128-bit
UUID consumes 17 of the 31 available payload bytes, peers commonly carry
service UUIDs in the scan response instead. Filtering could therefore hide
the exact field being matched on. The cost is repeated reports from nearby
devices, which stop as soon as the appliance is found; the report handler is
idempotent (it ignores reports once the state is no longer `Scanning`), so
duplicates are harmless.

**Threading**: see the "NimBLE host task" bullet under Runtime model above
— NimBLE callbacks hand off through a bounded queue; `App::Run()`'s main
task is the only thing that ever drains it, on the same task and the same
`Poll()` shape as `EncoderInput`. `DialState` is never touched by BLE
callbacks directly, now or in the future.

**`ui/` and `display/` need no changes** for any of this — `UiManager` has
no BLE awareness, and BLE has no reason to know about the panel or SPI
bus.

If a future milestone's real BLE work (scanning, connecting, GATT
discovery, reconnect/backoff) turns out not to fit cleanly into this
shape, that's a sign the module boundary is wrong and worth revisiting
rather than working around.
