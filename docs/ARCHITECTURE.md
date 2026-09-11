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
               (progress ring, amount/checkmark, water type, hint, two tiny
               connectivity glyphs, a travelling highlight during
               Dispensing). Takes an lv_display_t* and a DialState to
               render from -- no knowledge of SPI, GPIO, esp_lcd, or the
               encoder. Contains no business logic: for any given
               DialState, Render() always produces the same widget
               content/visibility (M11's small set of LVGL animations are
               started/stopped on state *edges* Render() detects via its
               own last-rendered-state bookkeeping, not business logic of
               their own -- see ui_manager.cpp's own comments).
  grohe_ble/   BleManager owns the NimBLE host stack lifecycle and the BLE
               connection state machine (Idle/Initializing/Scanning/
               Connecting/Discovering/Ready/Disconnected/Backoff -- only
               Idle->Initializing->Scanning is reachable so far; see "BLE"
               below). GroheClient is a thin facade over it -- the one
               class app/ is allowed to talk to for BLE, following the
               same "app never reaches past the facade" pattern as
               display/encoder/ui. CredentialsProvider (M13.1) is the
               injected-dependency seam GroheClient reads BLE credentials
               through -- LocalCredentialsProvider (a gitignored,
               compile-time header, developer-only) and
               NvsCredentialsProvider (M13.1's own, runtime-provisioned,
               NVS-backed) both implement it; see "BLE" below.
  settings/    DialSettingsStore: NVS-backed default dispense amount,
               encoder step size, and default water type (M13.1) --
               overrides on top of dial_state.hpp's own compile-time
               constants, which remain the fallback for a device with
               nothing stored. Depends only on dial_state/ (for
               WaterType), the same "bottom of the graph" shape
               dial_state/ itself has -- no dependency on app/ or any
               other component.
  provisioning/ ProvisioningServer (M13.2): a second, independent
               esp_http_server instance (its own port -- see
               "Provisioning (M13.2)" below for why not OTA's) exposing
               POST /provision, gated by its own shared secret
               (ProvisioningSecretProvider), that writes Grohe BLE
               credentials into grohe_ble::NvsCredentialsProvider. Depends
               on grohe_ble/ (for Credentials/NvsCredentialsProvider) and
               time_service/ (for WifiConnection, its third consumer) --
               no dependency on components/ota/, which this milestone
               does not touch.
  app/         App: the composition root, plus DialController -- the
               "Application Controller" that owns the one DialState
               instance and applies the interaction rules (amount
               clamping, water-type toggle, dispense logging) in response
               to EncoderEvents. App itself only wires
               EncoderInput::Poll() -> DialController::HandleEvent() ->
               UiManager::Render(), and separately drains
               GroheClient::Poll() the same way; it contains no rules of
               its own. As of M13.1, App also owns the one
               DialSettingsStore and NvsCredentialsProvider instance and
               feeds the former's Values() into DialController via
               ApplySettings() once, at startup. As of M13.2, App also
               owns the one ProvisioningServer instance.
  firmware_info/ Read-only build metadata (version, git commit/branch/
               dirty, build date/time) -- see "Firmware metadata (M12.3)"
               below. Depends only on esp_app_format; nothing else in this
               project depends on it except app/, which logs it at boot.
  time_service/ SntpTimeProvider (a one-shot SNTP time source) and
               WifiConnection -- the one place this firmware brings Wi-Fi
               up or down. See "Wi-Fi connectivity" below. No dependency
               on anything else in this project.
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
app --> firmware_info --> (esp_app_format)
app --> time_service --> (esp_wifi, esp_netif, esp_event, lwip, nvs_flash)
app --> settings  --> dial_state
app --> provisioning --> grohe_ble, time_service
grohe_ble --> time_service  (GroheClient's SntpTimeProvider; takes
                             WifiConnection& from app, doesn't construct
                             it -- see "Wi-Fi connectivity")
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

## Display orientation

The enclosure mounts the LCD module physically rotated from its native
upright orientation (a mechanical decision, not a firmware one) — the
firmware output has to rotate to match. This has changed four times
across enclosure revisions so far; the full derivation history is kept
below since each earlier result is what the next attempt was actually
derived from, not a discarded dead end, and since it's also the
evidence backing every entry in the rotation table below.

**Centrally configured, as a single constant**: `board::kDisplayRotation`
(`components/board/include/board/board_config.hpp`), one of
`board::DisplayRotation::{k0, k90, k180, k270}` (0/90/180/270 degrees
clockwise). Changing that one line is sufficient to re-target a
different physical mounting — no UI, widget, or rendering code needs to
change, or knows the concept exists. Today it's `k90`, confirmed on the
physical device to be correct for the current enclosure.

All four values have been built and verified on real hardware, each
producing a clean rotation with no unintended mirroring — see "Rotation
table" below for the per-value evidence.

**Applied entirely at the panel-driver level, exactly once**, in
`Gc9a01Display::Init()` (`components/display/gc9a01_display.cpp`),
immediately after `esp_lcd_panel_init()` and before
`esp_lcd_panel_disp_on_off()`:

```cpp
const MadctlRotation madctl_rotation =
    ToMadctlRotation(board::kDisplayRotation);
ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_, madctl_rotation.swap_xy));
ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_, madctl_rotation.mirror_x,
                                      madctl_rotation.mirror_y));
```

`ToMadctlRotation()` (anonymous namespace, same file) is a pure lookup
table from `board::DisplayRotation` to the `swap_xy()`/`mirror()`
arguments that produce it — see "Rotation table" below for its exact
contents. `esp_lcd_panel_swap_xy()`/`esp_lcd_panel_mirror()` are
standard `esp_lcd` API (`esp_lcd_panel_ops.h`), already implemented in
`lcd_panel_gc9a01.c`'s `panel_gc9a01_swap_xy()`/`panel_gc9a01_mirror()`
(this project's own ported panel driver, see "Component map" above).
Both write the GC9A01's MADCTL register (`LCD_CMD_MADCTL`, the same
command `0x36` the vendor init table already sets once at boot) with
the MV/MX/MY bits — real hardware rotation, not a software workaround.
No other code changed: LVGL still renders into the exact same 240×240
logical coordinate space it always has (this panel is square, so
swapping X/Y doesn't even change the resolution LVGL is told about —
no `lv_display_set_resolution()` call is needed), and every widget,
animation, and the boot splash keep the coordinates they already had
regardless of which `board::DisplayRotation` is selected. The panel
driver alone is responsible for physically placing each pixel LVGL
writes at logical `(x, y)` onto the correct rotated location on the
glass.

**Rotation table**, since this panel's own confirmed-upright baseline
MADCTL — set once by the vendor init table (`kInit_36` in
`gc9a01_vendor_init.cpp`) — is `MX=1, MY=0, MV=0` (plus the unrelated
`BGR` bit, untouched by any of the calls below), not the generic
`MX=0` baseline most public MADCTL rotation tables assume, so none of
these values could be copied from one of those tables directly. Every
row is a MADCTL state this project's own enclosure bring-up already
put on real hardware (see "Full revision history" below) — none are a
fresh theoretical derivation:

| `DisplayRotation` | `swap_xy` | `mirror(x, y)` | MADCTL (MV, MX, MY) | Hardware evidence |
| --- | --- | --- | --- | --- |
| `k0`   | `false` | `(true, false)` | `0, 1, 0` | The vendor init table's own MADCTL value — confirmed upright with no calls at all. |
| `k90`  | `true`  | `(false, false)` | `1, 0, 0` | The configuration shipping today (revision 4 below) — confirmed correct amount, correct (clockwise) direction, no mirroring. |
| `k180` | `true`  | `(true, true)` | `1, 1, 1` | Confirmed, while chasing a 90° rotation on revision 1 below, to produce a clean 180° rotation. |
| `k270` | `true`  | `(true, false)` | `1, 1, 0` | Confirmed, on revision 1 below, to rotate the correct (90°) amount but in the wrong (counter-clockwise) direction — i.e. exactly `k270`'s definition (270° clockwise == 90° counter-clockwise). |

The one MADCTL state that shows up in the revision history but isn't in
this table, `swap_xy(true)` + `mirror(false, true)` (`MV=1, MX=0,
MY=1`, from revision 3 below), is deliberately excluded: it's the
correct 90°-clockwise *amount* and direction, but mirrored — a
reflection, not one of the four clean rotations this abstraction
offers.

**Full revision history**, since each entry in the table above traces
back to one of these four enclosure revisions:

1. An enclosure revision called for a 90° clockwise rotation.
   `swap_xy(true)` + `mirror(true, false)` (`MV=1, MX=1, MY=0`) was
   derived as the best candidate and hardware-confirmed to rotate the
   correct 90°, but in the wrong direction (counter-clockwise instead
   of clockwise) — this is `k270` in the table above.
2. A later enclosure revision instead called for 180°.
   `swap_xy(true)` + `mirror(true, true)` (`MV=1, MX=1, MY=1`) had
   already been tried, while chasing (1), and was hardware-confirmed to
   produce a clean 180° rotation — directly reused here once the
   requirement changed to 180° for real. This is `k180` in the table
   above.
3. The enclosure reverted to the 90° rotation from (1). Hardware
   feedback on that first 90° attempt was conclusive: correct rotation
   amount, wrong direction (needed a further 90° counter-clockwise).
   The opposite-direction 90° candidate, reachable from (2)'s
   confirmed-180° state by flipping `MX` instead of `MY`, is
   `swap_xy(true)` + `mirror(false, true)` (`MV=1, MX=0, MY=1`) — and
   was hardware-confirmed to be the correct rotation amount *and*
   direction, but horizontally mirrored (text only readable as a
   mirror image). This is the excluded reflection state above, not a
   row in the table.
4. With `swap_xy(true)` held fixed throughout (three of its four
   possible `mirror()` pairings now hardware-characterized: `(true,
   true)` is 180°; `(true, false)` is 90° the wrong direction; `(false,
   true)` is 90° the right direction but mirrored), the one remaining
   untested pairing, `mirror(false, false)`, is what removes that
   mirroring while keeping the same rotation — this is `k90` in the
   table above, and the value `board::kDisplayRotation` is set to
   today.

**Encoder behavior is unaffected by any of this, structurally, not just
in practice.** The rotary encoder is a separate GPIO-ISR quadrature
input path (`encoder::RotaryEncoder`/`EncoderInput`, see "Why GPIO-ISR
(not PCNT) for the encoder" below) with no dependency on, or shared code
with, the SPI display output path — physically remounting the *display*
changes nothing about how encoder pulses are decoded into
`EncoderEvent::kRotateCw`/`kRotateCcw`. No encoder file was touched for
this change.

## Flash layout (M9)

Verified against the real hardware (`esptool flash_id`), not assumed from
Kconfig: this board's ESP32-C3 has 4 MB of embedded flash. Through M8, the
project used ESP-IDF's built-in `partitions_singleapp_large.csv` (a single
1500K `factory` app partition) — comfortable until M9 added Wi-Fi/lwIP/
WPA-supplicant, which alone cost ~448 KB of image size and left that
partition at 2% free. A custom `partitions.csv` (`CONFIG_PARTITION_TABLE_CUSTOM`)
replaces it:

```
nvs,        data, nvs,     ,        0x6000   (24K, unchanged)
otadata,    data, ota,     ,        0x2000   (8K, fixed by ESP-IDF's OTA data format)
phy_init,   data, phy,     ,        0x1000   (4K, unchanged)
ota_0,      app,  ota_0,   ,        1984K
ota_1,      app,  ota_1,   ,        1984K
```

Two equal, 64K-aligned `ota_0`/`ota_1` slots rather than a single, larger
`factory` partition — even though OTA isn't implemented yet — because a
single-slot layout can never be safely OTA-updated later (a failed/
corrupted write would overwrite the only running image), and this
project's own history (Wi-Fi's one-time ~448 KB jump) shows flash usage can
move in large, lumpy steps. Adopting the OTA-ready shape now, before any
real field data lives in NVS, avoids a second, disruptive partition
migration whenever OTA (an explicit product-roadmap item) actually ships;
`idf.py flash` already writes into `ota_0` exactly as `factory` would have,
so no OTA code is required for this table to work today. At the current
image size (~1465 KB), each slot has ~519 KB of headroom — comparable to
what the original single-partition table had before the Wi-Fi addition,
just doubled up safely. See `partitions.csv`'s own comment for the exact
sizing arithmetic.

## Firmware metadata (M12.3)

Metadata only -- no product behaviour changes. Goal: any build can be
identified exactly from its own boot log, with no manual step before a
release build and no dependence on anything only present on one
developer's machine.

**Single source of truth for the version string**: `version.txt` at the
repository root (currently `v1.0.0-dev`) -- ESP-IDF's own build system
(`tools/cmake/project.cmake`) already reads this file first, ahead of
`git describe`, into `PROJECT_VER`, and embeds it into the app image's
`esp_app_desc_t.version` field (the same structure a future OTA/rollback
check would read to compare versions between slots -- see `esp_app_desc.h`).
Bumping the version for a release is exactly one line in one file; nothing
else in this codebase defines or duplicates a version string. Before this
milestone, the project had no `version.txt`, so `PROJECT_VER` silently
drifted with every commit via `git describe` (visible in earlier build
logs as e.g. `v0.2.0-3-g6b8d74b-dirty`) -- deliberate now, not accidental.

**New `components/firmware_info/`** exposes this (plus what ESP-IDF's own
descriptor doesn't track) through a small, dependency-free API:

```cpp
namespace firmware_info {
const char* Version();    // esp_app_desc_t.version, e.g. "v1.0.0-dev"
const char* GitCommit();  // build-time git short SHA, or "unknown"
const char* GitBranch();  // build-time git branch, or "unknown"/"HEAD"
bool        GitDirty();   // uncommitted changes at build time?
const char* BuildDate();  // esp_app_desc_t.date  (__DATE__, local time)
const char* BuildTime();  // esp_app_desc_t.time  (__TIME__, local time)
}
```

`Version()`/`BuildDate()`/`BuildTime()` are thin wrappers around
`esp_app_get_description()` -- ESP-IDF is the one source of truth for
those three, nothing here re-derives or duplicates them. `GitCommit()`/
`GitBranch()`/`GitDirty()` have no ESP-IDF equivalent, so
`components/firmware_info/GenerateGitHeader.cmake` generates a small
header (`firmware_info_git.h`, private to `firmware_info.cpp` -- nothing
else includes it) at *build* time, not just CMake-configure time: a new
`add_custom_target(... ALL ...)` re-runs on every `idf.py build`
invocation (two fast `git` subprocess calls), so a new commit is reflected
on the very next build, not just after a full reconfigure. The script
only actually rewrites the header when its content changes, so a build
with no new commits doesn't force a spurious recompile of
`firmware_info.cpp`. If `git` isn't installed, or `SOURCE_DIR` isn't a git
repository at all (e.g. a source archive with no `.git`, or any other
environment that isn't a developer's own checkout) -- the script degrades
to `"unknown"`/clean rather than failing the build. Nothing here is
Espressif- or NimBLE-specific, so this component has no `REQUIRES` beyond
`esp_app_format`, and it will keep working unchanged under GitHub
Actions or any other CI runner that has `git` on `PATH`.

**Boot log** (`app::App::Run()`, before any other subsystem's `Init()`):

```
Grohe Dial
Firmware: v1.0.0-dev
Commit: ba1155f (main)
Built: Aug  4 2026 20:15:30
```

`(main, dirty)` when the working tree had uncommitted changes at build
time. `BuildDate()`/`BuildTime()` are the build machine's local wall-clock
time as ESP-IDF's `__DATE__`/`__TIME__`-based fields report them -- no
timezone is attached by either ESP-IDF or this code, so nothing here
should be read as UTC. This is deliberately a second, concise log distinct
from ESP-IDF's own built-in, more verbose "Application information:" block
(`esp_app_format`'s `init_show_app_info`, already printed automatically
before `app_main()` even runs) -- both read from the same underlying
`esp_app_desc_t`, so there is no duplicated *source*, only a second,
shorter *rendering* of it for this project's own boot log.

## Wi-Fi connectivity

**`time_service::WifiConnection`** is the one place this firmware brings
the Wi-Fi STA interface up or down. Extracted out of what used to be
`SntpTimeProvider`'s own private, one-shot connect/retry/teardown state
machine so that any *other* consumer needing Wi-Fi could reuse the same
session instead of running a second, independent connect/retry
implementation against the one physical radio this chip has. Two
consumers today, with two different acquisition lifetimes:
`SntpTimeProvider`'s one-shot boot-time burst (`Acquire()`d, then
`Release()`d once time sync finishes) and `ota::OtaServer`'s permanent
acquisition (`AcquireAsync()`d once at boot, never `Release()`d -- see
"OTA (M12)" below) -- an earlier, HTTPS-based OTA engine was a second
consumer for a time too (see git history around 2026-08 for that former
design), removed before this one was built.

**Design, kept general on purpose:** reference-counted
(`Acquire()`/`AcquireAsync()` increment, `Release()` decrements) rather
than assuming exactly one user -- the connection is brought up on the
first acquisition and torn down (`esp_wifi_stop`/`deinit`,
`esp_netif_destroy`) only once every acquirer has released, so a future
second consumer could reuse this exact class safely, without
modification, the moment one exists again. Two acquisition styles built
in for the same reason:
- `AcquireAsync(on_ready, on_failed)` -- non-blocking, callback-driven.
  `SntpTimeProvider::Init()` uses this, preserving M9's own explicit
  design goal ("no dedicated task, no blocking wait" -- Wi-Fi/SNTP must
  never delay the rest of `App::Run()`'s startup sequence).
- `Acquire(timeout)` -- blocking, returns whether connected. Currently
  unused (no consumer needs a blocking wait today), kept because it's
  a thin, already-correct wrapper over the same underlying core, not
  because anything calls it right now.

Both styles share one underlying event-driven core (`WIFI_EVENT`/
`IP_EVENT` handled on the default event loop's own task) plus a
`FreeRTOS` event group as the cross-task signal both styles read:
`AcquireAsync()`'s callbacks fire from the event handler directly;
`Acquire()` is a thin `xEventGroupWaitBits()` wrapper. Neither style
spawns a task of its own. "Connected" (`on_ready()`, or `Acquire() ==
true`) strictly means both `WIFI_EVENT_STA_CONNECTED` (L2 association)
*and* `IP_EVENT_STA_GOT_IP` (DHCP complete) have happened for this cycle
-- a private `sta_connected_` flag, set on `STA_CONNECTED` and cleared on
`STA_DISCONNECTED`, is a hard precondition `GOT_IP` checks before
resolving, so a caller that gets a positive result is guaranteed a
genuinely usable IP connection.

**Ownership** lives at `app::App` (the composition root, where every
other cross-cutting shared resource already lives), injected by
reference into `GroheClient`'s constructor -- the identical
dependency-injection pattern already used one level down
(`SntpTimeProvider` never owned `WifiCredentialsProvider` either).

```
app::App
 |-- time_service::WifiConnection wifi_connection_
 |     depends on: time_service::LocalWifiCredentialsProvider
 |
 |-- ota::OtaServer ota_server_{wifi_connection_, ota_secret_provider_}
 |     depends on: ota::LocalOtaSecretProvider
 |
 '-- grohe_ble::GroheClient grohe_client_{wifi_connection_}
       '-- time_service::SntpTimeProvider time_provider_(wifi_connection_)
```

`App::Run()` calls `wifi_connection_.Init()` once, early -- before
`grohe_client_.Init()`, since `GroheClient::Init()` (via
`SntpTimeProvider::Init()`) is the first thing to actually acquire a
connection through it.

**A cycle's own teardown can poison the next cycle** -- a real,
hardware-found bug, worth keeping as documentation even with a single
consumer today, since it applies to any repeated acquire/release
sequence, not just a multi-consumer one. `TearDownWifi()`'s
`esp_wifi_stop()` posts `WIFI_EVENT_STA_DISCONNECTED` asynchronously
whenever the STA was still associated -- an ordinary side effect of
stopping, indistinguishable on the wire from a real disconnect. That
post can only be processed once the event loop task's own call stack
(which, for `Release()`, is running the *same* teardown that generated
it) unwinds back to its dispatch loop -- by which point `ref_count_` is
already 0 and `esp_wifi_deinit()` has already run. If
`HandleWifiOrIpEvent()` had no way to recognize this as stale, it would
retry via `esp_wifi_connect()` against an already-deinitialized driver,
fail synchronously, and set `kFailedBit` -- with nobody having acquired
anything. The *next* real `Acquire()`/`AcquireAsync()` call would then
see that leftover `kFailedBit`, either failing immediately itself or
(worse) causing its own genuine `WIFI_EVENT_STA_START`/`STA_CONNECTED`/
`GOT_IP` sequence to be silently discarded by the existing "already
resolved this cycle" guard, since that guard only checks *whether* a bit
is set, not *which* cycle set it.

The fix: `HandleWifiOrIpEvent()` checks `ref_count_.load() == 0` before
anything else. While nobody holds an acquisition, no event can be for a
live cycle -- it can only be leftover noise from the teardown that just
dropped `ref_count_` to 0 -- so it's dropped unconditionally, before it
can touch `retry_count_`, `sta_connected_`, or either bit.

## OTA (M12)

Wi-Fi firmware updates -- `idf.py build` on a dev machine, then push the
result to the dial over the local network, instead of USB for every
iteration. USB (`idf.py flash`) remains the initial-installation and
recovery mechanism unconditionally; nothing about OTA changes it or
depends on it being unavailable.

**Why not the M12.4 HTTPS design this replaces:** an earlier OTA engine
(`esp_https_ota`, TLS) was built, reached real hardware, and failed
there with `mbedtls_ssl_setup()` returning `MBEDTLS_ERR_SSL_ALLOC_FAILED`
-- root-caused to heap fragmentation: `free=25648,
largest_free_block=9216` bytes, not enough contiguous space for
mbedTLS's ~16.4 KB `in_buf` handshake allocation, on an ESP32-C3 with no
PSRAM and Wi-Fi + BLE (NimBLE) + LVGL all concurrently resident. That
engine was fully reverted (see git history and the "Wi-Fi connectivity"
section above). This design sidesteps the failure mode entirely rather
than working around it: **plain HTTP, no TLS, no mbedTLS allocation of
any kind in the transport.**

**Transport**: `ota::OtaServer` (`components/ota/`), a small
`esp_http_server` instance with two routes:
- `GET /version` -- unauthenticated, returns
  `firmware_info::Version()`/`GitCommit()`/`GitBranch()` as plain text.
  Read-only, non-sensitive; used by `scripts/ota.sh` to confirm the
  device is reachable and to report what it rebooted into.
- `POST /ota` -- requires an `X-OTA-Token` header matching the
  configured shared secret (see "Security" below); on success, streams
  the request body straight into `esp_ota_write()`, `kRecvChunkSize`
  (4 KiB) at a time, via `esp_ota_get_next_update_partition()` --
  **never buffers the full image**, matching this chip's memory
  constraints the same way M3.2's display partial-buffer decision did.
  `esp_ota_begin()` is given the exact `Content-Length` up front, so it
  erases only the flash region this image needs, not the whole
  partition.

**Flow**, matching `esp_ota_ops`'s own idiom exactly (`esp_https_ota`
itself is just a thin wrapper around the same calls -- this component
calls them directly instead):
`esp_ota_get_next_update_partition()` (find the inactive slot) ->
`esp_ota_begin()` -> repeated `httpd_req_recv()`/`esp_ota_write()` ->
`esp_ota_end()` (validates the image -- app descriptor magic byte, chip
ID, CRC) -> `esp_ota_set_boot_partition()` -> respond `200 OK` -> a
short `vTaskDelay()` so the response actually reaches the client ->
`esp_restart()`. Every failure path (`esp_ota_begin`/`_write`/`_end`/
`_set_boot_partition`, or a socket read error) responds with an
appropriate HTTP error and returns without ever calling
`esp_ota_set_boot_partition()` -- the currently running partition is
never touched by a failed update.

**Partitions**: reuses `ota_0`/`ota_1`/`otadata` exactly as they've
existed since M9 (see "Flash layout (M9)" above) -- no partition table
change was needed for this milestone.

**Rollback**: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`
(`sdkconfig.defaults`, reinstated for this milestone). A freshly-flashed
OTA image boots in `ESP_OTA_IMG_PENDING_VERIFY` state; if it crashes,
watchdogs, or loses power before confirming itself, the bootloader
automatically reverts to the previous working image on the next boot.
`ota::ConfirmBootValid()` (`ota_rollback.cpp`, wrapping
`esp_ota_mark_app_valid_cancel_rollback()`) is called once from
`App::Run()`, but deliberately *after* display, UI, encoder, and BLE
have all finished initializing -- not at the top of `Run()`, unlike
M12.4's equivalent call. This is a genuine improvement over that design:
confirming any earlier would mean a crash during startup itself (the
exact class of bug rollback exists to catch) could never trigger a
rollback, since the image would already have been marked valid before
the crash occurred.

**Wi-Fi lifetime**: `OtaServer` is `WifiConnection`'s second consumer
(see "Wi-Fi connectivity" above), but with a different acquisition
shape than `SntpTimeProvider`'s one-shot burst -- `Init()` calls
`AcquireAsync()` once and never `Release()`s, so Wi-Fi stays connected
for the whole process lifetime whenever a device is powered, keeping
the OTA endpoint reachable on demand rather than only in a brief window
after boot. This is still non-blocking and non-fatal: `Init()` returns
immediately regardless of how long Wi-Fi takes to associate or whether
it ever does, and every other subsystem (display, encoder, BLE) starts
up exactly as before -- OTA is not a runtime dependency, the same
principle `WifiConnection` already enforces for every consumer. A
long-lived Wi-Fi outage exceeding `WifiConnection`'s own cumulative
retry budget (`kMaxWifiRetries`, currently 5) leaves the OTA endpoint
unreachable until the next reboot resets the retry cycle -- acceptable
for a local, developer-facing feature; documented rather than solved,
matching this class's own existing "stale event" precedent.

**Security**: a single shared secret (`ota::OtaSecretProvider`,
gitignored `ota_secret_local.hpp` -- mirrors
`grohe_ble::CredentialsProvider`/`time_service::WifiCredentialsProvider`
exactly, same local-header pattern, same "empty means disabled, never
means unauthenticated" default), checked with a best-effort
constant-time comparison and sent as a header value, never logged.
**This is not Internet-grade security**: no TLS, so the token is sent
in the clear and the firmware image itself is not encrypted in transit;
no rate limiting; no replay protection. It is appropriate for a
trusted local Wi-Fi network -- the threat model is "don't let an
unauthenticated device on my LAN flash this dial," not "resist an
attacker who can already observe my Wi-Fi traffic." See `SECURITY.md`.
`GET /version` intentionally has no auth -- it returns nothing
sensitive, and both `scripts/ota.sh` and casual debugging benefit from
it working without the token.

**Developer workflow**: `scripts/ota.sh <device-ip>` -- locates
`build/grohe_dial.bin`, reads the shared secret from the same
`ota_secret_local.hpp` the firmware itself reads (one file, one place
to edit), uploads with `curl`, checks the HTTP status, then polls
`GET /version` for up to 30 s to confirm the device came back and
report what it's now running.

### App-task watchdog (M16.3)

`App::Run()`'s own steady-state loop registers itself with ESP-IDF's
Task Watchdog Timer (`esp_task_wdt_add(nullptr)`, placed *after*
`ota::ConfirmBootValid()` -- the init sequence before that point has
its own legitimately variable-length waits, Wi-Fi association and BLE
bring-up, that aren't individually instrumented with their own resets,
so registering earlier would risk a false trip on a slow-but-healthy
boot) and feeds it (`esp_task_wdt_reset()`) as the first statement of
every loop iteration, comfortably inside the TWDT's 5 s default timeout
at the loop's normal ~20 ms cadence.

The TWDT itself is already an ESP-IDF default
(`CONFIG_ESP_TASK_WDT_EN`/`_INIT`); what M16.3 adds on top is
`CONFIG_ESP_TASK_WDT_PANIC=y` (`sdkconfig.defaults`), without which a
trip only logs a warning and does nothing further. With it, a future
bug that hangs the app task triggers an automatic panic-reset instead
of freezing the dial forever -- and composes correctly with the OTA
rollback guarantee above: a hang before `ConfirmBootValid()` leaves the
image unconfirmed, so the reset this triggers also rolls the image
back, not just reboots into the same bad build.

Mechanism proven on real hardware twice: once via a temporary diagnostic
build (an induced hang plus a temporary `esp_reset_reason()` readout on
`GET /version`, observing the reset reason transition `ESP_RST_SW` ->
`ESP_RST_TASK_WDT`), and again via a second, isolated hang test built
directly on top of the clean, current M16.3 source and flashed over USB,
observed across 4 consecutive cycles (`task_wdt` fires ~6s in, panics,
reboots cleanly, repeats). Both temporary changes were fully reverted
before their respective final builds. See
[`docs/m16_reliability_and_provisioning.md`](m16_reliability_and_provisioning.md)
§3/§7 for the full account.

## Provisioning (M13.2)

A second local-network HTTP endpoint, `POST /provision`, lets Home
Assistant push this dial's Grohe BLE credentials once, rather than the
ESP32 ever implementing a second Grohe Cloud login itself. As of M15,
this is also where the dial's local HTTP API for the native Home
Assistant integration lives -- see
[`docs/m15_ha_integration.md`](m15_ha_integration.md) -- rather than a
third `esp_http_server` instance. Deliberately reuses OTA's transport/security *shape*
(`esp_http_server`, plain HTTP, shared-secret header, constant-time
compare, fail-closed on an empty secret) without touching
`components/ota/` itself at all, and without reusing its secret.

**Why a separate `esp_http_server` instance, not OTA's**:
`esp_http_server` binds one instance per TCP port; `ota::OtaServer`
already owns port 80 (`HTTPD_DEFAULT_CONFIG()`'s own default). Adding
`/provision` as a second route on that same instance would mean either
modifying `components/ota/` (out of scope for this milestone) or a new
cross-component coupling neither `ota/` nor the new
`components/provisioning/` needs. `provisioning::ProvisioningServer`
therefore runs its own instance, on its own port
(`kProvisioningPort = 8080`, `provisioning_server.cpp`) -- still plain
HTTP, still no TLS, for the identical hardware reason OTA has none (see
"OTA (M12)" above).

**Request**: a single route, `POST /provision`, body
`{"user_id": "...", "preshared_key_base64": "..."}` -- the exact two
fields `grohe_ble::Credentials` already needs (see "BLE" below),
nothing else. Parsed with cJSON (ESP-IDF's bundled `json` component,
already a dependency of nothing else in this project but a standard,
already-vendored one) rather than a hand-rolled parser. The whole body
is read in one shot into a fixed, bounded buffer (`kMaxBodyLen`, 512
bytes) -- unlike OTA's multi-megabyte firmware upload (never buffered
in full), a two-field JSON body is small enough that streaming/chunking
would be needless complexity.

**Flow**, matching the order this milestone's own spec requires
(authenticate and validate the complete request *before* any NVS
write, exactly once, only on full success):
`IsAuthorized()` (header check) -> body-size check -> read body ->
`cJSON_Parse` -> field presence/type/length validation
(`grohe_ble::kMaxCredentialFieldLen`, shared with
`NvsCredentialsProvider`'s own on-disk field size so a too-long field
is correctly reported as `400`, not `500`) ->
`grohe_ble::NvsCredentialsProvider::Set()` (that single atomic blob
write -- see "BLE" below and M13.1's own history) -> `200 OK`. Any
failure before the `Set()` call touches no NVS state at all; a failure
*inside* `Set()` (a genuine NVS-level fault, not a bad request) leaves
whatever credentials were already stored completely untouched -- see
that function's own comment.

**Status codes**: `200` (stored), `400` (missing provisioning-secret
header value doesn't apply here -- that's `401` -- malformed JSON,
missing/empty/non-string/too-long fields), `401` (missing or wrong
`X-Provision-Token`), `500` (the request was valid but `Set()` itself
failed -- an NVS-level fault). No credential value ever appears in a
response body, success or failure.

**No hardware-button gating, deliberately**: this dial has exactly one
physical button, already assigned to dispensing/stop and the long-press
water-type cycle (see `app::DialController::HandleEvent()`) -- M13.2
adds no new interaction to it, and `/provision` is not behind any
button-hold or timed "pairing window" the way some IoT onboarding flows
work. It is reachable for as long as Wi-Fi is connected, full stop; the
security boundary is the provisioning token plus the local-network
assumption this whole project already makes for OTA, not a physical
gesture.

**Credential activation, no reboot required**: `NvsCredentialsProvider::
Set()` (M13.1) already updates its own in-memory cache immediately on a
successful NVS write, before returning. `grohe_ble::GroheClient` reads
`credentials_provider_.Get()` fresh on every single command it builds
(`SendCommand()`, `grohe_client.cpp`) -- nothing caches credentials at
BLE-connect time. The very next dispense/stop command sent after a
successful `/provision` call therefore signs with the new credentials,
whether or not a BLE connection is already established; wrong
credentials simply produce the appliance's existing, already-handled
`INVALID_HMAC` response, not a crash or a stuck state. `/provision`'s
own success response reports `"reboot_required": false` accordingly --
this was a deliberate choice to accurately describe M13.1's existing
architecture, not a new hot-reload mechanism built for this milestone.

**Security**: a single shared secret
(`provisioning::ProvisioningSecretProvider`, gitignored
`provisioning_secret_local.hpp` -- same local-header pattern as
`ota::OtaSecretProvider`/`grohe_ble::CredentialsProvider`/
`time_service::WifiCredentialsProvider`, same "empty means disabled,
never means unauthenticated" default) -- deliberately a *different*
secret from the OTA token, so a leaked/misused one never grants the
other's capability. Checked with the same best-effort constant-time
comparison OTA uses (duplicated, not shared, so `components/
provisioning/` has no dependency on `components/ota/` at all). **Not
Internet-grade security**, for the identical reasons already documented
for OTA: no TLS, so both the token and the provisioned credentials
themselves cross the network in the clear during that one request; no
rate limiting; no replay protection. This makes `/provision`'s own
security posture *more* consequential than OTA's -- OTA's payload
(firmware) being sniffed isn't a credential disclosure, whereas this
literally is -- so treat the provisioning token, and the local network
it's used on, accordingly. Appropriate for a trusted local Wi-Fi
network only; see `SECURITY.md`.

**Developer workflow**: `scripts/provision.sh <device-ip>` -- reads
`USER_ID`/`PRESHARED_KEY` from one of two sources, then `POST`s both to
`/provision` with `curl`, authenticated with the shared secret from the
same `provisioning_secret_local.hpp` the firmware itself reads (override
with `GROHE_DIAL_PROVISION_TOKEN`). Mirrors `scripts/ota.sh`'s own shape
(host via argument/`--host`/`GROHE_DIAL_HOST`, secret read from a
gitignored local header) but never prints `USER_ID`, `PRESHARED_KEY`, the
provisioning token, or (in `--from-cloud` mode) the Grohe Cloud
refresh/access token.

*Default source*: a local `grohe_blue_ble` checkout's own `.env`
(`../grohe_blue_ble/.env` by default, override with `GROHE_BLUE_BLE_ENV`)
-- just forwards what's already there, unchanged, no Grohe Cloud call.

*`--from-cloud` source*: fetches fresh credentials directly from the Grohe
Cloud via `scripts/grohe_cloud_refresh.py`, removing the `grohe_blue_ble`
checkout dependency entirely. This reuses
[`koproductions-code/grohe`](https://github.com/koproductions-code/grohe)
(MIT, `pip install grohe` -- see `scripts/requirements.txt`) for every
actual Grohe Cloud call; nothing in this project reimplements OIDC login
or token refresh. That package's own `client.py` is, with near certainty,
the literal source `grohe_blue_ble/docs/EVIDENCE.md` cites as `grohe/
client.py` for the `sub`-claim-as-`user_id` finding -- the code is
identical. Two lower-level pieces of that package are used directly rather
than through its main `GroheClient` (which mandates an email+password at
construction and has no public way to resume from an existing
refresh_token):

- `grohe.tokens.GroheTokens.get_tokens_from_credentials(email, password)`
  -- the interactive login itself (POSTs the Grohe account credentials to
  the real Keycloak login form at `idp2-apigw.cloud.grohe.com`, follows
  the app's own `ondus://.../oidc/token` redirect to fetch the initial
  tokens). Used **once**, interactively, by `scripts/grohe_cloud_bootstrap.py`
  (`python3 scripts/grohe_cloud_bootstrap.py`) -- prompts for email and a
  hidden (`getpass`) password, and persists only the resulting
  `refresh_token` to a gitignored file
  (`scripts/.grohe_cloud_refresh_token`, `chmod 600`, overridable with
  `GROHE_CLOUD_REFRESH_TOKEN_FILE` or bypassed entirely with
  `GROHE_CLOUD_REFRESH_TOKEN`). The password itself touches memory only
  for that one call and is never written anywhere.
- `grohe.tokens.GroheTokens.get_refresh_tokens(refresh_token)` -- `POST
  https://idp2-apigw.cloud.grohe.com/v3/iot/oidc/refresh`, JSON body
  `{"refresh_token": ..., "grant_type": "refresh_token"}`. Used on every
  `--from-cloud` run by `scripts/grohe_cloud_refresh.py` to turn the
  stored refresh_token into a fresh access_token, live-verified against
  the real endpoint during development (a deliberately invalid token
  correctly came back `GroheUnauthorizedError` from the real server, not a
  guess). If the cloud rotates the refresh_token in its response (normal
  OAuth behaviour), the local file is updated automatically. This is also
  the endpoint `github.com/gkreitz/homeassistant-grohe_sense` (a separate,
  independent reverse-engineering effort) uses -- corroborating it,
  distinct from `/v3/iot/oidc/token`, the endpoint the *official app*
  itself is configured with per its own decompiled `strings.xml`
  (`GroheWatersystems/resources/res/values/strings.xml`'s `tokenEndpoint`)
  via Google's AppAuth library. Both were checked; `/oidc/refresh` is the
  one this project's own scripts use, since it's what the reused library
  actually calls and what live-tested correctly.

`user_id` is decoded from the fresh access token's JWT `sub` claim with
the same call `GroheClient` itself uses internally
(`jwt.decode(access_token, options={'verify_signature': False})['sub']`,
`PyJWT` -- already a runtime need of `grohe`, see `scripts/requirements.txt`'s
own note on that package's incomplete declared dependencies, verified by
actually installing it into a clean venv rather than trusting its
`pyproject.toml`). The dashboard fetch (`GET /v3/iot/dashboard` with
`Authorization: Bearer <access_token>`) is the one call
`grohe_cloud_refresh.py` replicates directly instead of going through
`GroheClient.get_dashboard()` -- the alternative would mean reaching into
`GroheClient`'s own name-mangled private attributes from outside, which is
worse practice, not better reuse; this is a single already-documented,
already-authenticated GET, not auth/token logic. The real dashboard JSON
is `{"locations": [{"rooms": [{"appliances": [...]}]}]}`, not the flat
`{"appliance": {"presharedkey": ...}}` `grohe_blue_ble/docs/EVIDENCE.md`
shows as a simplified example -- confirmed two independent ways: reading
`grohe`'s own `dto/grohe_device.py` (`GroheDevice.get_devices()` walks
this exact tree) and reading the official Android app's decompiled DTOs
(`GroheWatersystems/sources/com/grohe/smarthome/core/network/dashboard/
model/*.java`). `grohe_cloud_refresh.py` walks that tree for the one
appliance carrying a `presharedkey` field, and refuses to guess (a clear
error, not a silent pick) if none or more than one is found.

### Home Assistant provisioning (M16.8)

The Home Assistant integration's own Options Flow
(`homeassistant/custom_components/grohe_dial/config_flow.py`'s
`GroheDialOptionsFlow`, reachable from an already-added dial's
"Configure" action) reuses every piece of the reference implementation
above through a new thin adapter module, `cloud.py` -- no Grohe Cloud
API logic is reimplemented a second time anywhere in this integration.
Three steps -- Cloud login (email/password, used once, never stored) ->
appliance selection (auto-skipped for a single-appliance account) -> the
dial's own provisioning token -- end in exactly one call to the
firmware's existing `POST /provision` endpoint above. **No firmware
change was needed**: that endpoint already accepted precisely
`{"user_id": ..., "preshared_key_base64": ...}`, which is exactly what
`cloud.py`'s `user_id_from_access_token()` + `list_appliances()`
produce. See
[`docs/m16_reliability_and_provisioning.md`](m16_reliability_and_provisioning.md)
§6/§8 for the full design and security rationale (secret separation
between the Cloud password, the Cloud refresh token, and the dial's own
provisioning/API tokens; no secrets in logs, verified by a dedicated
automated test).

### Stable device identity and `via_device` linking (M15.1/M15.3)

The dial's own Wi-Fi station MAC (`components/device_id/`, read via
`esp_read_mac(mac, ESP_MAC_WIFI_STA)` -- no Wi-Fi-driver dependency,
confirmed on real hardware to agree with `wifi_connection.cpp`'s own
`esp_wifi_get_mac()`-based boot log line) is exposed as a `"device_id"`
field on the existing `GET /api/status`, and used as the Home Assistant
config entry's `unique_id` (`f"mac-{device_id}"`) instead of the dial's
host/IP. An existing entry migrates to this in place --
`__init__.py`'s `_async_migrate_to_stable_unique_id()` renames the
*same* Device/Entity Registry rows (same `device.id`, same
`entity_id`s) rather than letting HA create new ones alongside orphaned
old ones, since `entity.py`'s own `dial_id` computation feeds both. See
[`docs/m15_completion.md`](m15_completion.md) §1 for the full mechanism
and why this runs from `async_setup_entry()` itself, not HA's formal
`async_migrate_entry`.

Separately, `config_flow.py`'s `GroheDialOptionsFlow` (above) now also
persists the Cloud `appliance_id` it already fetches during
provisioning into the config entry; `__init__.py` resolves it against
the Device Registry (`device_registry.async_get_devices()`, searching
across every config entry -- the target device, if any, belongs to a
*different* integration) to a `ha-grohe_smarthome` device, if one is
installed and owns a device for that same `appliance_id`, and links
this dial to it via `DeviceInfo(via_device_id=...)`. Fully soft in
every direction: no import of `ha-grohe_smarthome`'s own code, no error
if it isn't installed, no error if the link can't be resolved -- see
[`docs/m15_completion.md`](m15_completion.md) §3.

## MQTT / Home Assistant Discovery (M13.3, removed in M15)

Implemented in M13.3: `mqtt::MqttClient` (`components/dial_mqtt/`)
published the dial's own settings and BLE connection status via MQTT +
Home Assistant MQTT Discovery. **Removed in M15**, replaced by a native
Home Assistant integration talking to the dial over local HTTP -- see
[`docs/m15_ha_integration.md`](m15_ha_integration.md) for the current
architecture, API, and the reasoning behind the replacement (native
Config Entries/entities instead of MQTT Discovery's generic
auto-configuration, no broker dependency, real RAM/flash savings --
measured, not estimated, in that document). Kept here as a historical
pointer only; no MQTT code remains in the tree.

## Dispense UI (M11)

Implements `docs/ui/dispense_animation_mockups.md` (the frozen UI spec)
faithfully; the spec, not this section, is the source of truth for *why*
any of this looks the way it does. This section only records the handful
of implementation-level decisions the spec's illustrative mockups didn't
(and couldn't) pin down, plus one deliberate, documented simplification.

- **BLE readiness is observed twice, deliberately, rather than exposed by
  `GroheClient`.** `dial_state::ConnectionStatus` needs the same
  `ready_for_protocol_ && subscribed_` condition `GroheClient` already
  gates protocol writes on internally, but this milestone's scope excludes
  changing `GroheClient`/`BleManager`. Since `kReadyForProtocol` and
  `kSubscribed` are already public `BleEvent`s flowing through
  `GroheClient::Poll()`, `DialController` now tracks the same two flags a
  second time, purely for display (`HandleReadyForProtocol()`/
  `HandleSubscribed()`, see dial_controller.cpp). Both observations are
  driven by the same underlying NimBLE callbacks, so they cannot disagree
  in practice; if `GroheClient` ever grows a public readiness accessor,
  this duplication should be the first thing removed.
- **"Time Sync" vs. "No Time" is a UI-only timeout, not a firmware
  concept.** `TimeProvider`/`SntpTimeProvider` have no notion of "gave up"
  — SNTP either resolves or the Wi-Fi/SNTP window (M9) hasn't completed
  yet. `DialController::kTimeSyncTimeoutUs` (15 s, a generous multiple of
  that window) is the judgement call that turns one boolean
  (`HasValidTime()`) into the three-way `dial_state::TimeStatus` the
  frozen spec's state machine calls for. Documented here because it's the
  one number in this milestone with no basis in a measurement — it's a
  reasonable default, not a validated threshold.
- **Water-type de-emphasis is opacity-only, not a smaller font.** The
  frozen spec's mockups illustrate it as "smaller and/or lower contrast";
  this build only compiles Montserrat 14/28/48 (`sdkconfig`), so there is
  no smaller size between the label's existing 14 px and nothing. Opacity
  alone (100% → ~50%) satisfies the spec's own "and/or" wording without
  enabling a new font size.
- **The Stopping/Finished screen-wide text transitions are immediate
  swaps, not the timed cross-fades in the spec's motion table.** A
  literal 250–300 ms fade would require fading out several independently-
  changing labels, swapping their content, then fading back in — real
  animation-sequencing complexity for a transition that happens once per
  dispense and completes within a single `Render()` call either way. This
  is the one place this implementation's behaviour differs from the
  frozen spec rather than just filling in an unspecified detail; revisit
  if a future pass wants the literal fade.
- **`DispenseSession` gained one accessor, `DurationUs()`, and nothing
  else.** The timing model itself (`Start()`/`Stop()`/`Finished()`/
  `Remaining()`) is unchanged; `DurationUs()` exposes what the class
  already computes internally so `DialController::Tick()` can derive
  `delivered_ml = active_dispense_amount_ml * elapsed / duration` without
  `DispenseSession` needing to know about millilitres at all — exactly the
  extension point its own pre-M11 comment anticipated ("a future progress
  indicator... only ever touches this class").

## Status text polish (M11.1)

M11's screen carried two independent status lines: `hint_label_` (the
connection/time/dispense priority ladder) and a second, separate
`appliance_status_label_` showing the appliance's raw protocol response
code (`"APPL OK"` / `"APPL INVALID_HMAC"` / `"APPL CODE 7"` — internal
codes useful during M6–M9's reverse-engineering, never meant for a
production UI). M11.1 removes `appliance_status_label_` entirely (the
widget, its `Init()`, its `Render()` branch — dead code once nothing
displays it, not left invisible) and consolidates everything into
`hint_label_`'s own ladder. `DialController::HandleApplianceState()`
itself is unchanged — the decoded response still flows into
`dial_state::DialState`'s `appliance_response_*` fields, since that's data
capture, not UI; it's now logged (`ESP_LOGI`/`ESP_LOGW`) rather than
displayed, satisfying "those belong in logs, not in the UI" without
touching `GroheProtocol` or the fields themselves.

The new ladder is a genuine content rewrite, not just APPL-text removal —
confirmed by the fact that `TimeStatus::kSyncing` and `kUnavailable` (
previously "SYNCHRONISING..." vs. "TIME UNAVAILABLE", two different
messages) now both read "Synchronising..." : neither state is something
the user can act on differently, so showing a distinct "you're offline for
real" message for `kUnavailable` was more alarming than informative. Ready
+ Idle now shows no text at all (previously "PRESS TO POUR") — the ring
alone communicates readiness. Casing follows the milestone's own spec
verbatim: "PRESS TO STOP" (Dispensing) stays upper-case; every other
message is sentence case. `dial_state::TimeStatus` itself (the enum, and
`DialController`'s existing `kTimeSyncTimeoutUs` threshold logic) is
unchanged — only its two states' rendered text converged.

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
  below. As of M7, it also accepts a GATT write via `WriteCharacteristic()`
  (see "Sending commands" below) — M8 reuses this same method unchanged
  for a real dispense, exactly as it did for the M7 `stop()` probe. `Ready`
  remains unimplemented (protocol-level readiness distinct from GATT
  readiness). `Backoff` is implemented as of M11.1 — see "Automatic
  reconnect (M11.1)" below.
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
  As of M7, a successfully parsed response is also exposed structurally as
  `ApplianceState` (see "Appliance state" below), and the module gains
  `BuildStopPayload()`, a pure function mirroring the Python reference's
  `stop_command()`/`serialize_payload()` — it needs HMAC/credentials, so it
  pulls in the two new sibling modules below, the same way `protocol.py`
  imports `auth.py`. As of M8, `BuildStopPayload()` is a thin wrapper around
  a general `BuildDispensePayload(credentials, amount_ml, taste, timestamp,
  ...)` — the same payload-building path a real dispense command uses, not
  a second, duplicated one — and `ApplianceState` gains a `sequence`
  counter so `GroheClient` can tell which command a given acknowledgement
  answers (see "Dispensing" below).
- **`grohe_auth.hpp`/`.cpp`** (M7): HMAC-SHA256 and Base64 helpers (via
  mbedTLS), a direct port of the Python reference's `auth.py`, including its
  "free of any BLE or cloud logic" scoping — these are pure functions over
  plain bytes, with no knowledge of BLE, credentials storage, or the Grohe
  payload format itself.
- **`grohe_credentials.hpp`/`.cpp`/`nvs_credentials_provider.cpp`** (M7,
  extended M13.1): a small `CredentialsProvider` interface (`Get() ->
  const Credentials&`) so `BuildStopPayload()` never depends on *where*
  the user ID / pre-shared key come from. Two implementations today:
  `LocalCredentialsProvider` reads them from a gitignored local header
  (`credentials_local.hpp`; see `credentials_local.hpp.example` and
  `.gitignore`) — mirroring the Python reference's own gitignored `.env`,
  development-only, no secret ever committed — and `NvsCredentialsProvider`
  (M13.1), which reads/writes a runtime-provisioned pair from NVS (a
  single blob entry, `Set()`'s own comment explains why one write rather
  than two separate keys), falling back to an owned
  `LocalCredentialsProvider` whenever nothing has been provisioned yet.
  `GroheClient` takes a `const CredentialsProvider&` in its constructor
  (M13.1 — previously a hardcoded `LocalCredentialsProvider` member); which
  concrete implementation it actually gets is `app::App`'s decision, the
  composition root, the same as every other injected dependency in this
  codebase. Provisioning `NvsCredentialsProvider` itself -- the local
  HTTP endpoint that calls its `Set()` -- is `provisioning::
  ProvisioningServer` (M13.2); see "Provisioning (M13.2)" above.
- **`GroheClient`** is a thin facade over `BleManager` and `GroheProtocol`
  — the one class `app/` is allowed to talk to for BLE, mirroring how
  `app/` never reaches past `display`/`encoder`/`ui`'s own top-level
  classes either. `Poll()`'s public signature and behavior are exactly what
  M3.1 established (drain the lifecycle queue, forward to the caller's
  callback) — `app::App::Run()`'s call site has never changed. Internally,
  `Poll()` also drains `BleManager`'s separate characteristic queue and
  feeds `GroheProtocol` — this is the only class that knows both exist. As
  of M7, it also owned a one-time automatic sequencing decision (the
  `stop()` probe), mirroring how the Python reference's `client.py`
  orchestrates `ble.py`/`protocol.py` without either of them knowing about
  sequencing themselves. M8 replaces that automatic probe with
  caller-triggered commands — see "Dispensing" below — while keeping the
  same orchestration role: `RequestDispense()`/`RequestStop()` decide
  *whether* a command can be sent right now and disambiguate *which*
  command a later acknowledgement answers; `app::DialController` decides
  *why* (button presses), and never talks to `BleManager` or `GroheProtocol`
  directly.

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

**Automatic reconnect (M11.1):** every failure path in this class already
funneled through `FailConnection()` — connect timeout/failure, a
discovery-level GATT error, subscribe failure, or an unexpected
disconnect. M11.1 gives that one choke point a retry: it now also drains
`command_queue_` (see below) and calls `ScheduleReconnect()`, which enters
`BleState::kBackoff` and arms a one-shot `esp_timer` for
`kBackoffDelaysMs[backoff_index_]` — 1 s → 2 s → 5 s → 5 s → ... (clamped
at the array's last entry, never growing further), reset to the first
delay the moment `DiscoverNextServiceChrs()` reaches `kReadyForProtocol`
(this class's own pre-existing definition of "a successful connection").
The timer callback runs on the esp_timer task, not the host task, so it
does nothing but post a second `ble_npl_event` (`reconnect_event_`,
shaped identically to `command_event_`) onto NimBLE's own default event
queue — the exact same cross-task hand-off `WriteCharacteristic()` already
uses, preserving "every NimBLE call happens on the host task" with zero
new exceptions. Once that event fires, `HandleReconnectEvent()` calls
`StartScan()` again — the same entry point the very first connection
used, not a second, parallel connection path — guarded by `state_ ==
kBackoff` so a stray/late timer fire after the state already moved on for
an unrelated reason (e.g. a host reset landed mid-wait and `OnHostSync()`
already restarted scanning on its own) is a safe no-op. A host-level reset
(`OnHostReset()`/`OnHostSync()`) is untouched by this — it already has its
own working recovery path (NimBLE re-invokes `sync_cb` after a resync),
and none of the milestone's test scenarios (appliance power-cycled,
Bluetooth toggled, walked out of range) are host resets. There is no
attempt limit: reconnect retries indefinitely, since "the appliance is
powered off" and "still out of range" both look identical to an ordinary
scan that just hasn't found anything yet.

This closes a race this class's own pre-M11.1 comment had already
identified as latent: a `BleWriteCommand` carries only a value handle, not
a connection identity, so a write still queued when a connection failed
could — once something reconnects and hands out a numeric `conn_handle_`
that happens to match — be issued against the wrong peer.
`FailConnection()` now discards `command_queue_` unconditionally before
scheduling a retry, so every reconnect starts with an empty queue, the
same as any fresh connection.

`app::DialController`/`ui::UiManager` needed no protocol-level changes for
this: `dial_state::ConnectionStatus` already transitions
`kConnectionLost → kReady` purely from `BleManager` re-emitting
`kReadyForProtocol`/`kSubscribed` after a successful reconnect, via
entirely pre-existing M11 logic. The one gap was the milestone's own "a
short visible indication (~1 second)" requirement — left alone,
`connection_status` would sit at `kConnectionLost` for the *entire* retry
duration, however long BleManager's own backoff takes, never reading
"Connecting..." until the moment it was already nearly back. `DialController`
gained a small, purely cosmetic `connection_lost_until_us_` hold (same
shape as the pre-existing `finished_until_us_`): `HandleConnectionLost()`
(re)arms it on every `kConnectionFailed` event, including a retry-attempt
failure that arrives after the dial had already moved on to
"Connecting..."; `Tick()` flips `connection_status` to `kConnecting` once
it elapses. This dial-side ~1 s hold is intentionally decoupled from
BleManager's own (much longer, backing-off) retry loop — it only decides
how long the *acknowledgement* is shown, not how long the underlying
retry actually takes.

**Sending commands** (M7 introduced the mechanism; M8 uses it for real):
M6's own hardware validation, cross-checked against the Python reference's
`client.py` (its `_connect()` never issues a GATT read — only
`start_notifications()`), established that the Grohe read characteristic
carries data *only* as an acknowledgement to a write. There is no passive
appliance-state broadcast to read. M7 exploited this with exactly one
write per connection — the confirmed, idempotent `stop()` command
(`amount=0`, `taste=0`) — purely to elicit that acknowledgement as
protocol-activation infrastructure. M8 replaces the *automatic* firing
with genuine, caller-triggered commands (see "Dispensing" below), reusing
the identical send path for both a real dispense and `stop()`.

`GroheClient` gates every command on two independent signals, both
required: `BleEventType::kReadyForProtocol` (discovery finished, so
`GroheProtocol` has cached the write handle) and `BleEventType::kSubscribed`
(the CCCD write finished, so a response actually has somewhere to arrive).
Hardware evidence showed these are not ordered — `ReadyForProtocol` can
fire *before* the subscribe write completes — so gating on either alone
would risk sending before notifications are enabled and losing the reply.
Both flags, plus `pending_command_` (M8: at most one command outstanding
at a time — see "Dispensing" below), reset on `kConnectionFailed` so a
future reconnect starts clean.

Sending the write itself does not add a synchronized `conn_handle_` or any
other cross-task read to `BleManager` — every member it has ever had
remains touched exclusively by the NimBLE host task, with the one
exception being that `WriteCharacteristic()` is *callable* from the app
task. It only ever enqueues a `BleWriteCommand` onto a new `command_queue_`
and posts a `ble_npl_event` to NimBLE's own default event queue
(`nimble_port_get_dflt_eventq()`, already serviced forever by
`HostTask()`'s `nimble_port_run()`) — the same primitive NimBLE itself uses
to marshal work onto that task. The actual `ble_gattc_write_flat()` call,
and its read of `conn_handle_`, happens once that event fires, on the host
task, exactly like every other GATT call this class makes.

**Appliance state** (M7): the one confirmed, decoded field set, `ApplianceState
{received, timestamp, response_code, is_success}`, flows
`GroheProtocol` → `app::DialController::HandleApplianceState()` →
`dial_state::DialState` → `ui::UiManager` — `dial_state` has no dependency
on `grohe_ble` (see its own `CMakeLists.txt`), so `DialController` is the
one place that translates between the two, the same role it already plays
for `encoder::EncoderEvent`. Every decoded field, with its evidence trail:

| Field | Source | Confidence | Evidence |
|---|---|---|---|
| `timestamp` | Response payload's first `:`-separated field | Confirmed field exists; value uninterpreted | `protocol.py`'s own comment: "timestamp precision (seconds vs milliseconds) is *not* confirmed" — treated as an opaque integer, never interpreted, exactly as the reference does |
| `response_code` | Response payload's second `:`-separated field | ⭐⭐⭐⭐⭐ Confirmed | `docs/EVIDENCE.md`'s "Response Codes" table; ported byte-for-byte from `constants.py`'s `ResponseCode` enum in M6 |
| `is_success` | Derived: `response_code == SUCCESS (0)` | ⭐⭐⭐⭐⭐ Confirmed | `protocol.py`'s `ApplianceResponse.is_success` property |

What is **not** available, and why: appliance mode, idle/dispensing state,
filter status, CO₂ status, remaining lifetime, error/warning conditions,
capability flags, and firmware information are all either cloud-only
(`GET /v3/iot/dashboard`'s `appliance.status` field — not exposed over
BLE at all) or explicitly unconfirmed (`docs/TODO.md`'s "Future Ideas":
"Reverse engineer filter reset", "...CO₂ reset", "...diagnostics",
"...firmware update"). This isn't an implementation gap in this
milestone — it reflects the actual, current state of BLE reverse
engineering: the complete GATT hierarchy confirmed on hardware since M5
(`0x1800` Generic Access, `0x1801` Generic Attribute, the Grohe service's
READ/WRITE characteristics) has no other attribute to source any of these
from. Per the Python reference's own stated principle ("Implement only
behaviour that is confirmed by reverse engineering... mark assumptions
clearly"), these remain unknown rather than guessed.

**Dispensing** (M8): the first genuine appliance control. The dispense
payload is structurally identical to `stop()`'s — `userId:timestamp:
amountMl:taste:base64(hmac)`, `protocol.py`'s `create_request()` — just
with real `amount_ml`/`taste` instead of `0`/`0`; `BuildDispensePayload()`
is the one path both go through (`BuildStopPayload()` is now a thin
wrapper calling it with `0`/`WaterType::kUnknown`), so there is no second,
duplicated serialization to keep in sync. `taste` is written as a plain
`static_cast<int>(WaterType)` — `BuildDispensePayload()` has no per-value
switch or validation, so it already accepts every `WaterType` enumerator
equally; adding a new water type (M10) is purely a `dial_state`/
`DialController` change, not a `grohe_ble` one.

**Water types** (`grohe_ble::WaterType`, `constants.py`'s `WaterType`
`IntEnum`): `kUnknown = 0`, `kStill = 1`, `kMedium = 2`, `kSparkling = 3`,
`kHot = 4`, `kHotMixed = 5` — confirmed at the same ⭐⭐⭐⭐⭐ confidence as
the characteristic UUIDs and response codes (`grohe_blue_ble/docs/
EVIDENCE.md`'s "Water Types" table), sourced from the Android
application's own decompiled enums, not inferred from traffic. `kStill`/
`kSparkling` have been hardware-dispensed since M8; `kMedium` (M10) uses
the identical, already-proven payload/HMAC path, so nothing about
*sending* it is new — only that the dial now offers it as a third
selectable state. `kHot`/`kHotMixed` are out of scope: this appliance
model has no hot-water outlet, so there is nothing to select them for
(not a missing-evidence gap, just not applicable to Grohe Blue Home).
`dial_state::WaterType` deliberately only mirrors the three the dial
actually offers (`kStill`/`kMedium`/`kSparkling`, ordered to match the
long-press cycle) — `app::ToGroheWaterType()` remains the one place that
translates between the two enums, unchanged in shape since M8, just with
one more case.

*Disambiguating acknowledgements*: the response format carries no marker
of which command it answers, but `GroheClient` needs to know specifically
whether a given `SUCCESS`/error belongs to a dispense request or a stop
request. `ApplianceState::sequence` increments every time
`GroheProtocol::HandleNotification` parses a new response;
`RequestDispense()`/`RequestStop()` record it as a baseline the moment
their write is queued, and `TakeCommandOutcome()` reports a result only
once the sequence has advanced past that baseline — mirroring `client.py`'s
own `_execute()`/`_wait_for_acknowledgement()` pattern (drain, send, wait
for the *next* one) with a counter instead of a queue. Only one command
may be outstanding at a time (`GroheClient::pending_command_`); a second
request while one is in flight is rejected, not queued.

*Idle/Dispensing state machine* (`app::DialController`, driven from
`app::App::Run()` — see its own comment for why `DialController` never
touches `GroheClient` directly): short press while idle sends a dispense
request for the dialed amount; short press while dispensing sends `stop()`;
a request already in flight (`command_pending_`, distinct from
`dial_state::DispenseStatus` — a request can be pending before the
appliance has even acknowledged it) debounces a rapid second press. The
`Dispensing` state is entered only once the dispense command's `SUCCESS`
acknowledgement actually arrives (`HandleCommandOutcome()`), never on the
button press itself, and a non-success outcome leaves the status exactly
as it was — no invented state. A successful `stop()` acknowledgement, or
`BleEventType::kConnectionFailed` (`HandleConnectionLost()`, satisfying
"disconnect during dispense"), returns to `Idle` immediately.

*Physical dispense duration*: the appliance reports no dispense-completion
event over BLE, so returning to `Idle` automatically relies on the Python
reference's own empirically-measured timing model
(`grohe_blue_ble/docs/PERFORMANCE.md`'s "Physical Dispense Duration"
experiment, `examples/dispense_duration_test.py`,
`results/dispense_duration.csv`) — reused verbatim, not re-derived:

```
dispense_time ≈ startup_overhead + amount_ml * time_per_ml
```

with **startup_overhead ≈ 1.32 s** and **time_per_ml ≈ 0.0403 s/ml**,
measured across 100–1000 ml (the source document itself notes it is not
validated outside that range). Ported as `GroheProtocol::PredictDispenseDurationMs()`,
a pure function of `amount_ml`. The actual stopwatch is a small,
dedicated `app::DispenseSession` (`Start()`/`Stop()`/`Finished()`/
`Remaining()`) rather than raw timer fields on `DialController` — isolating
the model and its bookkeeping so a future progress indicator or
recalibration only touches this one class. `DialController::Tick()`,
called every `App::Run()` iteration, checks `Finished()` against
`esp_timer_get_time()` and returns to `Idle` once it fires.

**Time architecture (M9)** — replaces M8's temporary finding. M8's hardware
validation established that `time(nullptr)` returns seconds-since-boot on
this firmware (no RTC, no prior time source), which made every command
fail with `TIMESTAMP_EXPIRED`; substituting a real epoch (temporarily, for
that validation only, never committed) confirmed the credentials/HMAC
pipeline was otherwise correct. M9 replaces that gap with a genuine
production time source, behind a `TimeProvider` abstraction (new component
`components/time_service/`) so `GroheProtocol` never knows or cares where
the value came from:

```cpp
class TimeProvider {
 public:
  virtual bool IsValid() const = 0;
  virtual bool GetCurrentEpoch(uint32_t* out_epoch_seconds) const = 0;
};
```

`BuildDispensePayload()`/`BuildStopPayload()` (`grohe_protocol.hpp`) take a
`const TimeProvider&` instead of a raw timestamp, and call
`GetCurrentEpoch()` themselves; a `false` return is handled exactly like
an HMAC/buffer failure already was — reject the command, leave the output
buffer untouched, never fabricate a value. `time_provider.hpp` is the only
`time_service` header `grohe_protocol.hpp` includes.

*Chosen source: SNTP over Wi-Fi, one-shot.* This firmware has no RTC chip
and no existing Home Assistant (or other product) integration to source
time from — `grohe_blue_ble/docs/TODO.md`'s "Milestone 5 — Home Assistant"
is an unimplemented *future* idea for the *Python* library, not something
this firmware talks to. `SntpTimeProvider` acquires Wi-Fi once at boot,
syncs via SNTP, then releases it again — so Wi-Fi is never a runtime
dependency for anything else: the dial, BLE, and appliance control all
keep working exactly as before whether or not this ever succeeds. Once
synced, `time(nullptr)` keeps advancing correctly for the rest of the
session from the same tick source used elsewhere in this codebase,
confirmed on hardware across several dispense/stop commands issued tens
of seconds apart with monotonically increasing, genuine timestamps.
("Acquires"/"releases" is literal: the connect-with-retry state machine
and the actual `esp_wifi_stop()`/`esp_wifi_deinit()`/`esp_netif_destroy()`
teardown described in the rest of this section later moved into a
standalone `time_service::WifiConnection` — see "Wi-Fi connectivity"
above for the current design; the reasoning below is preserved as the
original M9 design record.)

*Entirely event-driven, no dedicated task.* `esp_event`'s default loop
already runs on its own internal task; Wi-Fi/IP events
(`WIFI_EVENT_STA_START`/`DISCONNECTED`, `IP_EVENT_STA_GOT_IP`) and SNTP's
own sync-notification callback drive every state transition, mirroring how
`BleManager` itself never spawns a task to poll NimBLE — it reacts to
NimBLE's own callbacks. The one wrinkle: SNTP's notification callback runs
on lwIP's own task, and the timeout `esp_timer` callback runs on
`esp_timer`'s own service task — neither is the same task WIFI_EVENT/
IP_EVENT handlers run on. Rather than adding a mutex, both re-post as a
custom event (`kInternalEventBase`) onto the *same* default event loop, so
`esp_event`'s own guarantee (every handler on one loop is serialized on
that loop's single task) means every piece of mutable state this class has
— retry counter, torn-down flag — is genuinely touched from one task only,
without its own lock. `valid_` is the sole exception: a `std::atomic<bool>`,
the one field actually read from a different task (the app task, via
`IsValid()`/`GetCurrentEpoch()`) — the same minimal, well-justified
reasoning already applied to `BleManager::conn_handle_` in M7.

*Credentials*: `WifiCredentialsProvider`/`LocalWifiCredentialsProvider`
mirror `grohe_ble`'s `CredentialsProvider`/`LocalCredentialsProvider`
exactly (gitignored local header + committed `.example`), injected into
`SntpTimeProvider` by reference rather than owned internally (as of
M12.5, into `WifiConnection` instead — see above), so a future
NVS- or Wi-Fi-provisioning-based implementation only requires writing a
new provider and changing one construction line in `GroheClient`.

*Failure handling*: a Wi-Fi/SNTP failure — wrong credentials, AP out of
range, SNTP server unreachable — never blocks or crashes anything else; it
just means `IsValid()` stays false, `GroheClient::HasValidTime()` reports
it, and `DialController`/`UiManager` show `"NO TIME"` (reusing the
existing appliance-status label, ahead of the normal `APPL ...` readout,
since it explains *why* nothing will authenticate). A hardware-confirmed
bug during self-review: `esp_wifi_connect()` can fail *synchronously*
(`ESP_ERR_WIFI_SSID` for an empty/invalid SSID, confirmed with placeholder
credentials) without ever firing `WIFI_EVENT_STA_DISCONNECTED` — if left
unchecked, this would have left Wi-Fi/lwIP resources allocated for the
entire session instead of tearing down immediately. Fixed by checking
`esp_wifi_connect()`'s own return value and tearing down right away on a
synchronous failure, not just an asynchronous disconnect.

*RF coexistence and RAM, both checked, not assumed*: the Grohe appliance's
BLE link is already marginal (−95 to −101 dBm since M5); pre-M9 hardware
logs already show roughly a 64% connection/discovery failure rate
independent of Wi-Fi (14 of 22 trials across M5–M8), so a single M9 trial
failing during the Wi-Fi sync window is within that existing variance, not
evidence of new interference — confirmed by running several more trials
with real Wi-Fi credentials, all completing BLE discovery/subscribe and
SNTP sync successfully with no crashes. Separately, linking Wi-Fi/lwIP/
WPA-supplicant in at all costs real, static RAM regardless of whether a
sync ever runs — the heap available before any runtime allocation dropped
from 121 KiB (M8) to 53 KiB — confirmed this is enough headroom for BLE
connect/discovery/dispense to keep working by testing repeated dispense
commands on real hardware after the change, not by assuming it would fit.

**Six hardware-discovered robustness issues in total across M5 through M9, all
found and fixed via repeated real-device trials, not by inspection alone**
(the appliance's signal is weak — around −95 to −101 dBm in M5's original
test conditions — which is what exposed the first two; the next two were
found later, at much better signal, purely from careful boundary reasoning
not matching a working reference closely enough; the fifth and sixth are
each unrelated to RF conditions — an initialization-order bug and an
unchecked return value, respectively):
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
5. `command_event_` (the `ble_npl_event` powering the M7 write path) was
   initially set up with `ble_npl_event_init()` *before* `nimble_port_init()`.
   On real hardware this crashed on every boot with a Guru Meditation Load
   access fault, before any `ble_manager` log line ever printed --
   `ble_npl_event_init()` draws from an NPL memory pool that
   `nimble_port_init()` itself sets up, so calling it any earlier reads
   that pool before it exists. Every other NimBLE call in `Init()` already
   happens after `nimble_port_init()` for the same reason; this one just
   wasn't obviously a NimBLE call by name. Fixed by moving the
   `ble_npl_event_init()` call to immediately after `nimble_port_init()`
   succeeds.
6. `SntpTimeProvider`'s `WIFI_EVENT_STA_START` handler called
   `esp_wifi_connect()` without checking its return value. With the
   placeholder empty Wi-Fi credentials, this failed *synchronously*
   (`ESP_ERR_WIFI_SSID`) and never fired `WIFI_EVENT_STA_DISCONNECTED` —
   the only event the retry/give-up logic was watching for — so nothing
   would ever have torn Wi-Fi/lwIP back down; confirmed on hardware by the
   complete absence of any further Wi-Fi log activity after the initial
   connect attempt. Fixed by checking `esp_wifi_connect()`'s own return
   value at both call sites and tearing down immediately on a synchronous
   failure, not only on an asynchronous disconnect.

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

### Appliance identity: address pinning and the identity probe (M15.2)

Before M15.2, `HandleDiscReport()` connected to the *first* device
advertising the Grohe service UUID -- correctness depended entirely on
no other Grohe Blue Home ever being in range at the same time. The
Grohe Cloud dashboard (verified by reading the real `grohe` package's
own DTOs and the decompiled official Android app's `BlueApplianceDto`
field list directly) carries no BLE MAC address at all, so no
pre-connection cloud-sourced filter is possible -- the only appliance
identity this system has anywhere is cryptographic: the appliance's own
HMAC verification of a signed command (`response_code=1`,
`"INVALID_HMAC"`, on a wrong key -- already proven in production by
M13.6's dispense-failure-feedback work).

- **`grohe_credentials.hpp`/`nvs_credentials_provider.cpp`**: a new
  `PinnedApplianceAddress`, stored under its own NVS key (`appliance_addr`,
  namespace `grohe_creds`) separate from the `{user_id,
  pre_shared_key_base64}` blob. `NvsCredentialsProvider::Set()` --
  unchanged call site, `/provision`'s existing handler -- now also
  erases this key in the same NVS commit whenever new credentials are
  stored: new credentials always invalidate any previous pin, whether
  this is first-ever provisioning or the physical appliance being
  replaced.
- **`BleManager`**: `SetAddressFilter()`/`ClearAddressFilter()`/
  `RejectCurrentPeerAndKeepScanning()`, all queue-based (mirroring
  `command_queue_`/`command_event_`'s own cross-task discipline exactly
  -- never touch `conn_handle_`/connection state off the host task).
  With a filter set, `HandleDiscReport()` ignores any advertisement not
  from that exact address, even one that also advertises the Grohe
  service UUID. Without one (the bootstrap/unpinned state), a small,
  session-only, never-persisted rejected-address list keeps a
  just-failed candidate from being immediately reselected once scanning
  resumes.
- **`GroheClient`**: `MaybeStartIdentityProbe()`, run every `Poll()`
  cycle once connected+subscribed+unpinned, sends one `stop()` command
  as a probe -- the one command this protocol already lets a client
  send with zero physical side effect. `ProcessIdentityProbeOutcome()`
  intercepts the response before it would ever reach the public
  `TakeCommandOutcome()` (App/DialController never know a probe
  happened): `INVALID_HMAC` -> `RejectCurrentPeerAndKeepScanning()`;
  anything else -> the key was genuinely verified ->
  `SetPinnedApplianceAddress()` + `SetAddressFilter()`. Retried every
  cycle rather than tied to the `kSubscribed` event's own single edge --
  real hardware showed that event can fire before SNTP has finished
  syncing, which would otherwise fail the probe's own `BuildStopPayload()`
  (M9's "no command without a valid clock" rule) and never pin an
  entirely legitimate appliance for the rest of that connection.

Hardware-verified end to end, including the rejection path: a real,
deliberately-corrupted pre-shared key made the real, only-available
Grohe Blue Home genuinely return `INVALID_HMAC`, which the dial
correctly rejected, disconnected from, and never reselected for the
remainder of that boot. See
[`docs/m15_completion.md`](m15_completion.md) §2 for the full evidence
and why that substitutes validly for a true two-physical-appliance
test.
