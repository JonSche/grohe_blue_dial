# Roadmap

Milestones for Grohe Dial. Later milestones are provisional — revisit and
reorder as the actual BLE contract with the Grohe Blue appliance becomes
known.

## M0 — Raw hardware bring-up ✅

Prove the panel, SPI wiring, and backlight are electrically sound before
any UI framework enters the picture.

- [x] New `components/bringup/` component (`bringup::ColorCycleTest`):
      raw `esp_lcd` + `esp_lcd_gc9a01` init, no LVGL, no `esp_lvgl_port`
      dependency at all.
- [x] Fills the whole screen with solid red/green/blue/white/black, one
      second each, in a loop.
- [x] `bringup::ColorCycleTest` stays in the tree as a standalone
      diagnostic, kept independent of `display`/`app` for exactly this
      purpose.

## M1 — Boot & display (LVGL) ✅

Bring the LVGL stack up on top of the panel bring-up from M0.

- [x] Project scaffolding: `board`, `display`, `encoder`, `ui`, `app`
      components with a one-way dependency graph (see
      [ARCHITECTURE.md](ARCHITECTURE.md)).
- [x] `display`: SPI bus + a ported GC9A01 driver (replacing the generic
      `esp_lcd_gc9a01` registry driver) + a self-owned LVGL v9 integration
      (no `esp_lvgl_port` — see [ARCHITECTURE.md](ARCHITECTURE.md)'s
      "Runtime model" for the single-framebuffer and scheduler-timing
      details).
- [x] `ui`: static boot screen showing "Grohe Dial".
- [x] `encoder`: GPIO-ISR quadrature decode + button, wired up and logging
      in `app::App::PollInputs()` but not yet driving any UI.
- [x] No BLE — deliberately out of scope until M3.
- [x] `main.cpp` runs `app::App` (`REQUIRES app`); verified stable on
      hardware, no watchdog warnings.

## M2 — Input-driven UI ✅

Make the encoder and button actually do something on screen instead of
just logging.

- [x] `main.cpp` runs `app::App` (see M1).
- [x] `encoder::EncoderInput` replaces the raw poll-and-log diff check with
      typed events (`RotateCW`/`RotateCCW`/`ShortPress`/`LongPress`),
      still polled from `app::App`'s 50 ms loop.
- [x] New `dial_state::DialState` (amount_ml, water_type) is the single
      source of truth; `app::DialController` is the "Application
      Controller" that mutates it in response to encoder events.
- [x] `ui::UiManager` replaced with the first real dial screen: a circular
      progress ring (using the display's own round shape), large centered
      amount, water type, and a static "Press to pour" hint --
      `Render(const DialState&)` is a pure function of state, called after
      every change.
- [x] Interaction: rotate = ±100 ml (clamped 100-2000), short press = logs
      a dispense request (no BLE), long press = toggles still/sparkling.
- [ ] Backlight/idle handling (dim or blank after inactivity,
      `Gc9a01Display::SetBacklight()` already supports on/off) -- deferred,
      out of scope for this milestone.
- [ ] Real product decision on whether this is the final screen layout, or
      just the first cut -- this milestone is deliberately a single static
      screen, no menus/pages (see M4/M5 for BLE-driven content and visual
      polish).

## M3 — BLE client foundation

Introduce connectivity without yet committing to what it controls.

- [x] Architecture design review: NimBLE (not Bluedroid), single
      `components/grohe_ble/` component (not a generic `ble/` +
      Grohe-specific split — rejected as premature abstraction, see
      [ARCHITECTURE.md](ARCHITECTURE.md#ble-grohe_ble)), queue-based
      threading model mirroring `EncoderInput::Poll()`.

### M3.1 — BLE infrastructure ✅

NimBLE host init and the state-machine skeleton only — no scanning,
connecting, or discovery yet.

- [x] `components/grohe_ble/`: `BleManager` (NimBLE host lifecycle, state
      machine, bounded event queue) + `GroheClient` (thin facade), wired
      into `app::App` (`Init()` failure logs and continues without BLE;
      `Poll()` drained from the main loop, log-only for now).
- [x] Minimal NimBLE Kconfig (`CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1`,
      `CONFIG_BT_NIMBLE_MAX_BONDS=1` — this firmware only ever talks to
      one peripheral).
- [x] Verified on hardware: `Idle -> Initializing -> Scanning` transitions
      correctly, host-synced event flows through the queue to `App`'s
      poll loop, BLE and the display coexist with no boot loop or
      stability issues (see M3.2).
- [ ] Scanning, connecting, GATT discovery, reconnect/backoff — this is
      where most of the real complexity will live; deferred to a future
      milestone once the Grohe Blue's actual GATT contract is known.

### M3.2 — Display: LVGL partial rendering ✅

Not originally planned as part of M3 — enabling BLE surfaced a real
memory conflict (a Root Cause Analysis traced it to ESP32-C3's IRAM/DRAM
aliasing under NimBLE, not fixable from this project's side; a follow-up
architecture review found the display's full-screen framebuffer was an
inherited implementation choice, not a requirement — see
[ARCHITECTURE.md](ARCHITECTURE.md#runtime-model)).

- [x] `Gc9a01Display` switched from a 115,200-byte full-frame buffer
      (`LV_DISPLAY_RENDER_MODE_FULL`) to a 14,400-byte, 30-row partial
      buffer (`LV_DISPLAY_RENDER_MODE_PARTIAL`) — no changes needed to the
      flush callback or the GC9A01 panel driver, both already area-
      agnostic. Verified on hardware: BLE and display now coexist, UI is
      pixel-identical, no artifacts or stability issues.

## M4 — Grohe Blue control

Depends on the BLE/GATT contract of the target appliance being defined.

- [ ] Read appliance state (mode, filter status, whatever the real device
      exposes) over BLE and reflect it in the UI.
- [ ] Write commands from the dial (encoder + button) back to the
      appliance.
- [ ] Error/timeout handling for a lost or rejected connection.

## M5 — Product polish

- [ ] Settings persistence (NVS) — last-used mode, pairing info, etc.
- [ ] OTA updates.
- [ ] Power/sleep management appropriate for a countertop device.
- [ ] Real visual design pass on `ui/` (the M1 boot screen is a
      placeholder, not the intended final look).
