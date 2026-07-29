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

## M2 — Input-driven UI

Make the encoder and button actually do something on screen instead of
just logging.

- [x] `main.cpp` runs `app::App` (see M1).
- [ ] Replace the poll-and-log loop in `app::App` with real event handling
      (e.g. an input event queue rather than a 50 ms diff check).
- [ ] `ui::UiManager` grows beyond a static label: a simple focus/selection
      model driven by encoder turns, confirm via button press.
- [ ] Backlight/idle handling (dim or blank after inactivity,
      `Gc9a01Display::SetBacklight()` already supports on/off).
- [ ] Decide the actual first screen(s) of the dial UI (this is a product
      decision, not just a technical one).

## M3 — BLE client foundation

Introduce connectivity without yet committing to what it controls.

- [ ] New `components/ble/` component (`GroheBleClient`) — NimBLE GATT
      client: scan/connect, service discovery, a small callback-based API.
      No dependency on `display`/`encoder`/`ui`, matching the existing
      component pattern.
- [ ] `app::App` owns a `GroheBleClient` instance and surfaces connection
      state through `ui::UiManager::SetStatusText()`.
- [ ] Reconnection/backoff handling; this is where most of the real
      complexity will live.

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
