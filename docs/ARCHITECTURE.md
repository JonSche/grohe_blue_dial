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
  `Disconnected`/`Backoff`).
  `Idle -> Initializing -> Scanning -> DeviceFound -> Connecting ->
  Connected -> DiscoveringServices -> ReadyForProtocol` is reachable end to
  end: the host starts, syncs, scans until the Grohe service UUID is seen,
  connects, negotiates MTU, and walks the full GATT hierarchy (all primary
  services, then all characteristics per service, one service at a time).
  As of M6, it also reports every discovered characteristic (see
  `PollCharacteristicEvents()` below) and automatically subscribes to
  notifications on the Grohe read characteristic — see "GATT discovery"
  below. `Ready` and `Backoff` remain unimplemented — no protocol read/write
  handling or reconnect logic yet.
- **`ble_constants.hpp`** holds the BLE protocol constants: the Grohe
  service UUID (M4) and, as of M6, the read/write characteristic UUIDs.
  Every one of these appears exactly once in the codebase, so the protocol
  surface stays reviewable in one place.
- **`GroheProtocol`** (M6) interprets the raw GATT data `BleManager` reports
  as the Grohe application protocol: caching the read/write characteristic
  handles by UUID, and logging every received payload — structured as
  `{timestamp, responseCode}` where it parses as the confirmed format, raw
  hex otherwise (a payload that doesn't parse is not an error; see "GATT
  discovery" below). It owns no NimBLE types and makes no NimBLE calls —
  everything arrives as plain data via `BleManager::PollCharacteristicEvents()`.
- **`GroheClient`** is a thin facade over both `BleManager` and
  `GroheProtocol` — the one class `app/` is allowed to talk to for BLE,
  mirroring how `app/` never reaches past `display`/`encoder`/`ui`'s own
  top-level classes either. `Poll()`'s public signature and behavior are
  exactly what M3.1 established (drain the lifecycle queue, forward to the
  caller's callback) — `app::App::Run()`'s call site has never changed.
  Internally, `Poll()` also drains `BleManager`'s separate characteristic
  queue and feeds `GroheProtocol` — this is the only class that knows both
  exist.

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

**Subscribing to notifications** (M6): which characteristic to
auto-subscribe to is deliberately the one piece of Grohe-specific knowledge
inside `BleManager` — an intentional, narrow exception to "no protocol
knowledge in the transport layer", mirroring `kGroheServiceUuid`'s
identical role in the scan filter (M4). The alternative — `GroheClient`
calling back into `BleManager` from the app task once `GroheProtocol`
recognizes the read characteristic — was the original plan, but was
rejected during implementation: every member `BleManager` has ever touched
(`state_`, `conn_handle_`, `services_`, ...) has been host-task-only since
M3.1, with no synchronization around any of it, and a cross-task call would
have been the first exception. Keeping the trigger inside `BleManager`
means every NimBLE call this class makes still happens on the one task
that has ever touched its state.

Enabling notifications is: find the characteristic's Client Characteristic
Configuration Descriptor (CCCD, UUID `0x2902`) via `ble_gattc_disc_all_dscs()`,
then write `{0x01, 0x00}` to it via `ble_gattc_write_flat()` — the standard
sequence, matching `blecent`'s own `blecent_read_write_subscribe()`. The
search range needs the *next* characteristic's declaration handle as its
upper bound (descriptors belong to the characteristic immediately
preceding them, up to but not including the next declaration), but NimBLE
reports characteristics one at a time in handle order, so that boundary
isn't known when the read characteristic itself is reported. The search is
therefore deferred (`pending_subscribe_val_handle_`/
`descriptor_search_started_`) until either the next characteristic in the
same service arrives, or — if the read characteristic was the last one —
service discovery for that service completes, at which point the service's
own end handle is the correct boundary.

A GATT-level failure anywhere in this sequence (missing CCCD, a discovery
or write error) disconnects cleanly via the same `FailConnection()` path
as every other transport failure. A received payload that doesn't parse
into the confirmed protocol format is a different kind of thing entirely —
not a transport failure, and not disconnected on: `GroheProtocol` logs it
as hex and the connection stays up, since this milestone deliberately does
not yet interpret every possible payload shape.

**Four hardware-discovered robustness issues in total across M5 and M6, all
found and fixed via repeated real-device trials, not by inspection alone**
(the appliance's signal is weak — around −95 to −101 dBm in M5's original
test conditions — which is what exposed the first two; the last two were
found later, at much better signal, purely from careful boundary reasoning
not matching a working reference closely enough):
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
3. The descriptor search range was initially computed as
   `[val_handle + 1, service_end_handle]` — reasoning from first
   principles about where a CCCD "should" be, rather than from a working
   reference. This reproducibly failed to find a CCCD that a direct
   Python/CoreBluetooth query confirmed does exist, because the range was
   both wrong at the start (should start *at* `val_handle`, not after it)
   and wrong at the end (sweeping into the *next* characteristic's own
   declaration/value and misreporting them as descriptors, rather than
   stopping just before it). Fixed by matching ESP-IDF's own bundled
   `blecent` example (`apps/blecent/src/peer.c`'s `chr_end_handle()`)
   exactly, which required deferring the search as described above.
4. The CCCD's own UUID was initially declared as a 128-bit Bluetooth Base
   UUID, on the reasoning that this device consistently uses that encoding
   for its own custom attributes (confirmed for the Grohe characteristics
   themselves). That reasoning didn't hold for the CCCD: this device
   encodes it in the standard compact 16-bit form (confirmed directly via
   this project's own descriptor-discovery log rendering it as `0x2902`,
   the short form `ble_uuid_to_str()` only produces for a true 16-bit
   type) — Bleak/CoreBluetooth's *separate* report of this same descriptor
   as the expanded 128-bit string is that library's own display
   normalization, not evidence about the actual over-the-air encoding.
   `ble_uuid_cmp()` rejects on a type mismatch before ever comparing the
   value, so the original 128-bit constant could never have matched.

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
