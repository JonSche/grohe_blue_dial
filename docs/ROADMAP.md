# Roadmap

Milestones for Grohe Dial. The Grohe Blue BLE contract (M3–M9) and the
core dispense experience (M11) are implemented and hardware-validated;
remaining milestones extend product scope (M10, M13) and developer/
production tooling (M12) on top of that foundation.

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
- [x] Backlight/idle handling (dim or blank after inactivity,
      `Gc9a01Display::SetBacklight()` already supports on/off) -- delivered
      in [M11.2](#m112--display-sleep) once there was a real dispense
      state machine to keep the display awake for.
- [ ] Real product decision on whether this is the final screen layout, or
      just the first cut -- this milestone is deliberately a single static
      screen, no menus/pages (see M11 for BLE-driven content and visual
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
- [x] Reconnect/backoff — deferred at the time (the Grohe Blue's actual
      GATT contract wasn't known yet, so there was no real protocol work
      to reconnect *for*); delivered once it was, in
      [M11.1](#m111--ui--connection-polish).

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

## M6 — Protocol Read Foundation ✅

Reusing the Python reference implementation as source of truth, establish a
reliable read path over the connection M5 already validated: cache the
Grohe READ/WRITE characteristic handles, subscribe to notifications on the
READ characteristic, and log every exchanged payload. No writes, no
business logic, no state changes on the appliance.

- [x] New `components/grohe_ble/{grohe_protocol.hpp,grohe_protocol.cpp}`:
      protocol-only knowledge, zero NimBLE transport includes (see
      [ARCHITECTURE.md](ARCHITECTURE.md#ble-grohe_ble)). Caches the
      READ/WRITE characteristic handles by UUID, parses the confirmed
      `timestamp:responseCode` response format ported directly from the
      Python reference's `protocol.py`, and logs every packet (direction,
      UUID, length, hex, and the structured form when it parses) — a
      payload that doesn't parse is logged as hex, not treated as an
      error.
- [x] `BleManager` gains a second, purpose-specific queue
      (`characteristic_queue_`/`BleCharacteristicEvent`), kept fully
      separate from the M3.1 lifecycle queue; auto-subscribes to the READ
      characteristic's notifications (CCCD discovery + write) the moment
      it's discovered, with the subscribe-trigger decision deliberately
      kept inside `BleManager` itself rather than called from the app task
      (see ARCHITECTURE.md's "Subscribing to notifications" section for
      the thread-safety rationale).
- [x] Error handling split as specified: transport/GATT-level failures
      (missing CCCD, discovery or write error) disconnect cleanly via the
      existing `FailConnection()` path; an unparseable payload is not an
      error and does not disconnect.
- [x] Two more hardware-discovered robustness bugs found and fixed during
      self-review (see ARCHITECTURE.md): an initial descriptor-search
      range that missed the appliance's real CCCD, and an initial
      CCCD UUID type mismatch caused by CoreBluetooth's 128-bit *display*
      normalization not reflecting the actual 16-bit wire encoding.
- [x] Verified on hardware across 6+ trials: characteristics cached,
      CCCD found, subscribe write succeeds, connection stays up, clean
      disconnect still works — zero crashes, zero queue-full events,
      zero regressions from M5.

## M7 — Appliance State Foundation ✅

M6's hardware validation revealed that meaningful appliance state is not
exposed passively over BLE — the Grohe read characteristic only ever
carries data as an acknowledgement to a write, never as an unprompted
status broadcast (confirmed against the Python reference's `client.py`
and our own GATT captures; see
[ARCHITECTURE.md](ARCHITECTURE.md#ble-grohe_ble)). This milestone is not
appliance control: the sole write it permits is the existing,
Python-validated, idempotent `stop()` command, used exclusively as a
protocol activation / state-elicitation mechanism to trigger the
acknowledgement flow ApplianceState is populated from — not as user-facing
functionality. No dispense commands, water selection, or other appliance
control are in scope.

- [x] Send the confirmed `stop()` payload (`amount=0`, `taste=0`) once
      per connection, purely to elicit the appliance's acknowledgement --
      HMAC-signed via a new `grohe_auth` module (mbedTLS) and a gitignored,
      swappable `CredentialsProvider` (`grohe_credentials`), mirroring the
      Python reference's `auth.py`/gitignored `.env`.
- [x] Decode the resulting notification into a structured
      `ApplianceState`, extending `GroheProtocol`'s existing
      `timestamp:responseCode` parser (ported from `protocol.py` in M6).
      Documented every field's source/confidence/evidence, and explicitly
      what appliance state is *not* available over BLE and why (see
      [ARCHITECTURE.md](ARCHITECTURE.md#ble-grohe_ble)).
- [x] `DialState` reflects the decoded `ApplianceState` (translated by
      `app::DialController`, which is the only place that bridges
      `dial_state`'s zero-dependency struct and `grohe_ble`'s type);
      `UiManager` displays it. Protocol parsing stays out of UI code.
- [x] `BleManager` gains a queued app-task → host-task write path
      (`WriteCharacteristic()`, a new `command_queue_` +
      `ble_npl_event`/`nimble_port_get_dflt_eventq()`) so the write reaches
      the host task without adding any synchronized/cross-task member to
      `BleManager` -- every member remains host-task-only, with zero
      exceptions, exactly as before M7.
- [x] A real hardware-discovered bug (an NPL event initialized before
      `nimble_port_init()`, crashing every boot) found and fixed during
      self-review -- see ARCHITECTURE.md's "Five hardware-discovered
      robustness issues" section.
- [x] Verified on hardware across 5 trials: `stop()` succeeds, the
      acknowledgement arrives and decodes (`INVALID_HMAC`, expected with
      placeholder credentials), `ApplianceState`/`DialState`/UI reflect it
      stably, the probe fires exactly once per connection every time, and
      clean disconnect still works -- zero crashes, zero regressions.

## M8 — First Successful Dispense ✅

M7 built the complete authenticated write pipeline but only ever exercised
it with an automatically-fired `stop()` probe. This milestone performs the
first genuine appliance control: replaces that probe with a real,
user-triggered dispense command, reusing the existing UI (encoder selects
amount, short press starts/stops) rather than adding menus or screens.
`stop()` remains available, now reachable as "press again while
dispensing" for testing and emergency cancellation.

- [x] `BuildDispensePayload()` (real `amount_ml`/`taste`) added to
      `GroheProtocol`, with `BuildStopPayload()` becoming a thin wrapper
      around it — one payload-building path for both, not two. `WaterType`
      ported from the Python reference's `constants.py`.
- [x] `ApplianceState` gains a `sequence` counter so `GroheClient` can tell
      which command (dispense vs. stop) a given acknowledgement answers —
      the response format itself carries no such marker.
- [x] `GroheClient` replaces M7's automatic probe with
      `RequestDispense()`/`RequestStop()` (at most one command outstanding
      at a time) and edge-triggered `TakeCommandOutcome()`.
- [x] `DialController` owns the Idle/Dispensing state machine: short press
      dispenses when idle, stops when dispensing; `Dispensing` is entered
      only on the dispense command's actual `SUCCESS` acknowledgement, never
      on the button press itself; a rejected/errored command leaves status
      unchanged (no invented state); `kConnectionFailed` forces a return to
      `Idle` ("disconnect during dispense").
- [x] Physical dispense duration (the appliance reports no BLE completion
      event) is predicted from the Python reference's own empirically-
      measured model (`docs/PERFORMANCE.md`'s "Physical Dispense Duration"
      experiment) — reused verbatim via `PredictDispenseDurationMs()` and a
      small, isolated `app::DispenseSession` stopwatch — not re-derived or
      approximated.
- [x] `UiManager` reuses the existing M2 hint label ("PRESS TO POUR" /
      "PRESS TO STOP"); no new screens or widgets.
- [x] Documented the dispense payload format, the ACK-disambiguation
      mechanism, the ported timing model and its validated range, and the
      state machine in [ARCHITECTURE.md](ARCHITECTURE.md#ble-grohe_ble).
- [x] Verified on hardware with real credentials: authenticated dispense
      writes succeed, the appliance physically dispenses (confirmed
      visually across several amounts), `stop()` mid-dispense returns to
      idle immediately and the appliance stops, and the UI transitions
      (hint label, auto-return-to-idle on the predicted timer) matched
      what was actually observed on the physical dial. "Disconnect during
      dispense" (`HandleConnectionLost()`) was verified by code review
      against the same `kConnectionFailed` mechanism already hardware-
      validated in M5–M7, not by a fresh live disconnect-mid-dispense
      test (impractical to induce non-destructively on this hardware).
- [x] Found during validation, not a code defect: this firmware has no
      real-time clock, so every command's timestamp was rejected as
      `TIMESTAMP_EXPIRED` until a real epoch was substituted (temporarily,
      for validation only, never committed) — which then produced genuine
      `SUCCESS` responses and real dispenses, confirming the
      credentials/HMAC pipeline is correct end to end. See
      [ARCHITECTURE.md](ARCHITECTURE.md#ble-grohe_ble)'s "No real-time
      clock" section — production use needs a real time source before
      this milestone's own commands will succeed outside a lab setting.

## M9 — Time Foundation ✅

M8's hardware validation found that authenticated commands only succeeded
with a temporary, hardcoded Unix epoch (never committed) -- this firmware
had no real time source at all. M9 replaces that gap with a genuine
production time source: SNTP over Wi-Fi, connected once at boot and fully
torn down again afterward, so Wi-Fi is never a runtime dependency for
anything else (BLE/appliance control and the UI all keep working exactly
as before whether or not it ever succeeds) -- an explicit product
requirement (any future Home Assistant integration stays optional, not a
dependency this milestone introduces).

- [x] New `components/time_service/`: an abstract `TimeProvider`
      (`IsValid()`/`GetCurrentEpoch()`) so `GroheProtocol` never knows
      where the value comes from, plus `SntpTimeProvider` -- entirely
      event-driven (no dedicated task, no blocking wait; mirrors how
      `BleManager` itself reacts to NimBLE's own callbacks rather than
      polling) -- and `WifiCredentialsProvider`/`LocalWifiCredentialsProvider`,
      mirroring `grohe_ble`'s own `CredentialsProvider` pattern exactly
      (gitignored local header + committed `.example`).
- [x] `BuildDispensePayload()`/`BuildStopPayload()` take a `TimeProvider&`
      instead of a raw timestamp; a time-unavailable result is rejected
      exactly like an HMAC/buffer failure already was -- never fabricated.
- [x] `DialController`/`UiManager` show a `"NO TIME"` status (reusing the
      existing appliance-status label) whenever authenticated commands
      can't succeed yet, ahead of the normal appliance-response readout.
- [x] Two real hardware-discovered bugs found and fixed during self-review
      (see [ARCHITECTURE.md](ARCHITECTURE.md#ble-grohe_ble)): an unchecked
      `esp_wifi_connect()` return value that would have leaked Wi-Fi/lwIP
      resources for the entire session on a synchronous connect failure,
      and a flash partition table left with only 2% free after Wi-Fi's
      ~448 KB image-size cost -- addressed with a custom, OTA-ready
      partition table (see [ARCHITECTURE.md](ARCHITECTURE.md#flash-layout-m9))
      rather than just enlarging the old single-partition layout, so OTA
      (M12) never needs a second, disruptive migration.
- [x] Verified on hardware with real Wi-Fi credentials: SNTP sync
      succeeds and tears down cleanly; BLE connection/discovery success
      rate showed no regression versus the existing ~64% baseline failure
      rate on this appliance's already-marginal RF link (M5-M8); repeated
      dispense/stop commands issued tens of seconds apart all produced
      genuine `SUCCESS` responses with correctly advancing real
      timestamps; confirmed no temporary/hardcoded timestamp code
      remains anywhere in the diff.

## M10 — Water Type Support ✅

Connection reliability (reconnect/backoff) and the dispense/UI experience
were both delivered ahead of schedule, in M11/M11.1 — the one remaining
functional gap on the appliance-control side was completing water-type
support itself: still/sparkling has worked since M2, and **Medium** was
the one remaining type to add.

- [x] Added **Medium** as the third selectable water type --
      `dial_state::WaterType` gains `kMedium`, ordered `kStill`/
      `kMedium`/`kSparkling` to match the new long-press cycle.
- [x] Long press now cycles Still → Medium → Sparkling → Still (was a
      two-way toggle) — an explicit three-case switch in
      `DialController::HandleEvent()`, not modular arithmetic, so a future
      fourth type fails to compile here rather than cycling silently wrong.
- [x] Updated the UI to support Still / Medium / Sparkling --
      `dial_state::WaterTypeLabel()` gains a `"MEDIUM"` case; `UiManager`
      itself needed no changes at all, since it already renders whatever
      `WaterTypeLabel()` returns (no redesign, no new widget).
- [x] Extended the payload mapping -- `grohe_ble::WaterType` already had
      `kMedium = 2` (ported from the Python reference's `constants.py` in
      an earlier milestone, never previously wired up to anything
      selectable); `app::ToGroheWaterType()` remains the single
      authoritative `dial_state::WaterType` ↔ `grohe_ble::WaterType`
      mapping, gaining one more case. `BuildDispensePayload()` itself
      needed no change: `taste` was already a plain
      `static_cast<int>(WaterType)`, not a per-value switch.
- [x] Documented the water-type evidence and mapping in
      [ARCHITECTURE.md](ARCHITECTURE.md#ble-grohe_ble)'s "Water types"
      section — confirmed at the same ⭐⭐⭐⭐⭐ confidence as the
      characteristic UUIDs and response codes (Android application enum
      decompilation, `grohe_blue_ble/docs/EVIDENCE.md`), not guessed.
- [ ] **Not yet done from this environment: hardware validation** (dispense
      Still, Medium, and Sparkling on the physical appliance; confirm no
      regression to Still/Sparkling or to stop). No physical hardware
      reachable here, same limitation M11/M11.1 ended on. Build verified
      clean (`idf.py build`, no new warnings) and every `WaterType` switch
      confirmed exhaustive (no `default:` label anywhere, so a missing
      case would already have failed to build).

## M11 — Dispense UI Implementation ✅

Implements `docs/ui/dispense_animation_mockups.md` (the frozen, approved UI
specification produced by this repo's own design-study sessions) exactly:
the ring holds one invariant meaning from Ready through Finished, delivered
volume counts up, a small travelling highlight communicates active flow
without the ring itself ever changing, and the two connectivity glyphs
(Interior Crown placement) present the full Connecting/Time Sync/Ready/
Connection Lost/No Time state machine. No BLE protocol, `GroheProtocol`,
`GroheClient`, `TimeProvider`, Wi-Fi, or `DispenseSession` timing-model code
changed -- see [ARCHITECTURE.md](ARCHITECTURE.md#dispense-ui-m11) for the
handful of implementation-level decisions the spec's illustrative mockups
didn't pin down (the BLE-readiness double-observation, the Time Sync/No
Time timeout, water-type de-emphasis via opacity rather than a smaller
font, and the one deliberate simplification: Stopping/Finished's screen-
wide text transitions are immediate swaps, not literal timed cross-fades).

- [x] `dial_state::DispenseStatus` gains `kStopping`/`kFinished`;
      `dial_state::ConnectionStatus`/`TimeStatus` added, replacing the
      plain `time_available` bool. `DialState` gains
      `active_dispense_amount_ml` (the ring's frozen invariant value,
      distinct from the live, still-rotatable `amount_ml`) and
      `delivered_ml` (the count-up value, rounded to the spec's 10 ml
      cadence before it ever reaches `UiManager`).
- [x] `DispenseSession::DurationUs()` added (the timing model itself is
      unchanged) so `DialController::Tick()` can derive delivered volume
      without `DispenseSession` needing to know about millilitres.
- [x] `DialController`: `HandleReadyForProtocol()`/`HandleSubscribed()`
      (BLE readiness, observed independently of `GroheClient` -- see
      ARCHITECTURE.md), a stop-request now enters `kStopping` optimistically
      and reverts to `kDispensing` on a rejected acknowledgement rather than
      silently doing nothing, and `Tick()` holds `kFinished` for ~400 ms
      (the checkmark) before returning to `kIdle`.
- [x] `UiManager`: the ring's fill is set once per state entry and never
      touched again mid-dispense; a second small `lv_arc` (the travelling
      highlight) oscillates within the dialled arc via `lv_anim_t` -- or,
      for small selections with no room to read as "travelling", breathes
      in place instead; the startup pulse, sync-sweep, and connecting-halo
      are likewise plain `lv_anim_t` animations, none of them touching the
      main ring. `LV_SYMBOL_OK` (not a raw Unicode check mark, which this
      build's compiled fonts don't include) is the Finished checkmark.
- [x] Self-review: every animated element traced back to a concrete LVGL
      primitive with a bounded redraw scope (matching the frozen spec's own
      "LVGL/ESP32-C3 implementability" verification table); no dead code
      (the old `time_available`-driven "NO TIME" branch is fully removed,
      not left stubbed); no debug logging added.
- [x] Verified on hardware: the full validation matrix (100/500/1000/
      2000 ml, completed/stopped/disconnected-mid-dispense) passed,
      together with the connection-reliability and display-sleep behaviour
      layered on top in [M11.1](#m111--ui--connection-polish)/
      [M11.2](#m112--display-sleep) -- dispense, stop, UI transitions,
      automatic reconnect, and the display staying awake through an
      entire pour all confirmed on the physical dial.

### M11.1 — UI & Connection Polish ✅

Two small improvements found during real hardware testing of M11 — a
polish pass, not a redesign: no protocol/auth/timing-model changes, no
dispense-behaviour changes, the M11 frozen UI spec's visual layout
untouched. See [ARCHITECTURE.md](ARCHITECTURE.md#status-text-polish-m111)
and [ARCHITECTURE.md](ARCHITECTURE.md#ble-grohe_ble)'s "Automatic
reconnect (M11.1)" for the implementation-level decisions.

- [x] Removed the separate `appliance_status_label_` ("APPL OK" / "APPL
      INVALID_HMAC" / "APPL CODE n") from the UI entirely — those are raw
      protocol response codes, useful during M6–M9's reverse-engineering,
      not appropriate for a production screen. The decoded response is
      still captured (`DialController::HandleApplianceState()`,
      unchanged) and now logged instead of displayed.
- [x] Rewrote `hint_label_`'s priority ladder to the milestone's own
      7-state table: Ready shows no text (was "PRESS TO POUR"); Connection
      Lost and no-time-yet both read in sentence case ("Connection lost",
      "Synchronising..." — the latter now shared by both `TimeStatus`
      states, previously two different messages); Dispensing/Stopping
      keep their existing meaning, with only Dispensing ("PRESS TO STOP")
      staying upper-case per the given spec.
- [x] `BleManager` gains automatic reconnect: every existing failure path
      (already funneled through `FailConnection()`) now schedules a retry
      via `BleState::kBackoff` and a one-shot `esp_timer` — the "1s → 2s →
      5s → 5s → ..." schedule this milestone asked for, reset on the next
      successful connection, retrying indefinitely. The retry itself
      re-invokes the existing `StartScan()` — no duplicated connection
      logic, no BLE-architecture changes (still every NimBLE call on the
      host task, via the same cross-task event hand-off
      `WriteCharacteristic()` already used).
- [x] Closed the `command_queue_` staleness race this class's own
      pre-M11.1 comment had already flagged as latent (a queued write
      surviving into a reconnected, differently-identitied connection) —
      `FailConnection()` now drains it before scheduling a retry.
- [x] `DialController` gained a small, purely cosmetic
      `connection_lost_until_us_` hold (same shape as the pre-existing
      `finished_until_us_`) so "Connection Lost" is genuinely shown only
      briefly (~1 s) before switching to "Connecting..." for the rest of
      BleManager's own (longer) retry loop — without this, the dial would
      have shown "Connection lost" for the entire reconnect, however long
      it took.
- [x] Verified on hardware: APPL text is gone and the Ready screen reads
      cleanly; the appliance was power-cycled, Bluetooth toggled off/on,
      and the dial walked out of range -- in every case it reconnected
      automatically with no duplicate connection attempts, and dispensing
      worked normally afterward.

### M11.2 — Display Sleep ✅

Backlight-only inactivity timeout, added as a follow-up once M11's
Dispensing/Stopping states existed to define what "active" means for a
dispense appliance -- the deliberately minimal version of the item M2
originally deferred: no LCD sleep command, no controller reset, no
reinitialisation, no LVGL pause, no framebuffer change, only
`Gc9a01Display::SetBacklight()`.

- [x] `app::App::Run()` gained a single `kDisplaySleepTimeoutMs = 60000`
      compile-time constant (the sole source of truth -- not a runtime
      setting) and a small `last_activity_us`/`backlight_on` pair, entirely
      local to the existing poll loop; no new task, no new timer, no
      display-driver change.
- [x] Activity is: any encoder event (rotate or press), or the dial being
      in `DispenseStatus::kDispensing`/`kStopping` -- the display must
      never sleep mid-pour or mid-stop, however long either takes.
      `kFinished`/`kIdle` are not activity by themselves; the timeout
      resumes counting once the dial is back to normal idle.
- [x] Verified on hardware: the display turns off after 60 s of genuine
      inactivity, rotating or pressing the encoder wakes it immediately
      and restarts the timer, and it stays on for the entire duration of
      a dispense/stop even when that exceeds 60 s.

## M12 — Development & Deployment

Developer- and release-focused tooling -- building, flashing, debugging,
and shipping this firmware repeatably -- rather than further product
features. Ordered by development priority, not release priority: reliable
debugging is worth more than OTA while firmware is still under active
development. The OTA-ready partition table itself was already delivered
in M9; this milestone is the OTA *mechanism* on top of it, not a second,
duplicate migration.

### Debugging

- [ ] JTAG/OpenOCD setup.
- [ ] VS Code launch configuration.
- [ ] Debugging documentation.

### Flashing

- [ ] Simple flashing workflow.
- [ ] Flash helper script(s).
- [ ] Automatic serial-port detection where practical.

### Build & Release

- [ ] Embed firmware version.
- [ ] Embed Git commit/version information.
- [ ] Release build configuration.

### OTA

- [ ] HTTPS OTA (`esp_https_ota`) using the existing M9 partition table
      (`ota_0`/`ota_1` + `otadata`).
- [ ] Rollback support.
- [ ] OTA documentation.

## M13 — Home Assistant Integration

Home Assistant extends the product; it never becomes a runtime dependency
of it -- the same "optional, not a dependency" principle M9 already
established for Wi-Fi/SNTP. Local BLE operation always has priority, and
the dial must remain fully usable -- dispensing, stopping, reconnecting,
everything it already does today -- whether or not Home Assistant is
present or reachable. Home Assistant's role here is primarily
configuration, diagnostics, and appliance status (CO₂, filter, firmware),
not basic dispensing, which the dial already handles entirely on its own.

- [ ] Home Assistant connectivity.
- [ ] Display filter status.
- [ ] Display CO₂ status.
- [ ] Display firmware information.
- [ ] Optional configuration through Home Assistant.
- [ ] Automatic entity discovery where appropriate.
- [ ] BLE operation must continue to work fully when Home Assistant is
      unavailable.

## v1.0 Release Criteria

What "version 1.0" means for this project -- the minimum bar for the
first production release, not a milestone in itself.

- [ ] M10 completed.
- [ ] M12 completed.
- [ ] M13 completed.
- [ ] Stable hardware validation.
- [ ] Reliable flashing workflow.
- [ ] Reliable debugging.
- [ ] No known critical defects.
- [ ] Complete documentation.
