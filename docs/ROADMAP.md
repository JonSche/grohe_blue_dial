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
      screen, no menus/pages (see M6/M7 for BLE-driven content and visual
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
- [x] Scanning — see M4 below.
- [x] Connecting, GATT discovery — see M5 below.
- [ ] Reconnect/backoff — deferred until the Grohe Blue's actual GATT
      contract is known and there's real protocol work to reconnect *for*.

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

## M4 — BLE advertisement discovery ✅

Find the appliance, and nothing more — no connecting, GATT discovery, or
authentication.

- [x] Active, continuous scanning (`ble_gap_disc()`), with advertisement
      payloads parsed by NimBLE's own `ble_hs_adv_parse_fields()`. Every
      report is logged with address, address type, RSSI, PDU type, local
      name, service UUIDs, manufacturer data and 128-bit service data.
- [x] `components/grohe_ble/include/grohe_ble/ble_constants.hpp`: the Grohe
      service UUID, defined exactly once, as the home for future
      service/characteristic UUIDs.
- [x] Detection ported from the Python reference implementation: match the
      advertised 128-bit service UUID. On a match, stop scanning, record the
      appliance address, transition to `DeviceFound`, and publish the event
      through the existing queue.
- [x] Verified on hardware: 5/5 independent 30 s trials discovered the
      appliance (in 1.0–2.4 s, at RSSI −99 to −101 dBm), zero advertisement
      reports logged after discovery in every trial, and the `DeviceFound`
      event reached the app task each time. ~8,000 advertisements from 34+
      distinct devices parsed with zero parse failures. The appliance
      advertises no local name, which is why UUID matching — not name
      matching — is the correct strategy.

## M5 — BLE connection & GATT service discovery ✅

Establish a connection to the appliance found in M4 and walk its full GATT
hierarchy. No protocol communication (reads/writes/notifications) yet.

- [x] `ble_gap_connect()` to the address `BleManager` recorded in M4
      (10 s timeout, matching the Python reference implementation's own
      `DEFAULT_CONNECT_TIMEOUT`), then `ble_gattc_exchange_mtu()`
      (non-fatal on failure), then `ble_gattc_disc_all_svcs()` and
      `ble_gattc_disc_all_chrs()` per service, sequentially — see
      [ARCHITECTURE.md](ARCHITECTURE.md#ble-grohe_ble) for the full
      procedure and state-machine detail.
- [x] New `BleState` values `Connected`/`DiscoveringServices`/
      `ReadyForProtocol`; new `BleEventType` values `ReadyForProtocol` and
      one shared `ConnectionFailed` (carrying the NimBLE/HCI status as
      `reason`) covering connect timeout/failure, discovery-level GATT
      errors, and unexpected disconnects alike — reported through the
      existing queue, no automatic reconnect.
- [x] Two hardware-discovered robustness bugs found and fixed during
      self-review (see ARCHITECTURE.md): an upstream ESP-IDF/NimBLE bug
      passing a dangling stack pointer as `cb_arg` during the controller's
      own automatic connection-reattempt behavior, and a stale-GATT-callback
      issue where a result from an already-superseded connection attempt
      could tear down a newer, good one.
- [x] Verified on hardware across ~15 trials: the full GATT hierarchy
      (`0x1800` Generic Access, `0x1801` Generic Attribute, and the Grohe
      service at `33f31bba-...`, handles `[10, 65535]`) discovered
      correctly and *identically* every time it completed — including the
      two characteristics matching the Python reference's `WRITE`
      (`1705`, properties `WRITE`) and `READ` (`1706`, properties
      `READ NOTIFY`) UUIDs exactly. The appliance's marginal RF link
      (−95 to −101 dBm) means not every trial completes within a fixed
      window — genuine link failures (`BLE_HS_ENOTCONN`, `BLE_HS_EBADDATA`)
      are reported cleanly through `ConnectionFailed` with no crash,
      corruption, or hang, exactly as this milestone's error-handling scope
      requires.

## M6 — Grohe Blue control

Depends on the BLE/GATT contract of the target appliance being defined.

- [ ] Read appliance state (mode, filter status, whatever the real device
      exposes) over BLE and reflect it in the UI.
- [ ] Write commands from the dial (encoder + button) back to the
      appliance.
- [ ] Error/timeout handling for a lost or rejected connection, plus
      reconnect/backoff.

## M7 — Product polish

- [ ] Settings persistence (NVS) — last-used mode, pairing info, etc.
- [ ] OTA updates.
- [ ] Power/sleep management appropriate for a countertop device.
- [ ] Real visual design pass on `ui/` (the M1 boot screen is a
      placeholder, not the intended final look).
