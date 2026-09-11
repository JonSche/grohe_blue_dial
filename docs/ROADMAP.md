# Roadmap

Milestones for Grohe Dial. The Grohe Blue BLE contract (M3–M9), the core
dispense experience (M11), water-type support (M10), and Wi-Fi OTA
updates (M12) are implemented and hardware-validated; the remaining
milestone (M13) extends product scope on top of that foundation.

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
- [x] **Resolved in practice**: this round layout, extended (not replaced)
      by M11/M11.1's BLE-driven content and visual polish, is what shipped
      -- no menus/pages were ever added, and it's the same design the
      README's own hero shot and screenshots show as of M16. Never revised
      after M11.1; "first cut or final" answered itself by staying
      unchanged across ten further milestones of real hardware use.

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
- [x] Verified on hardware: Still, Medium, and Sparkling all dispense
      correctly on the physical appliance, stop/cancel behaves correctly,
      and Still/Medium/Sparkling selection (the long-press cycle) works
      as expected -- no regression to the existing dispensing behavior.

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

## M12 — Development & Deployment ✅

Deliberately narrow scope: the one real requirement is **reliable
firmware updates over Wi-Fi, with USB retained as the initial-
installation and recovery mechanism** -- not a generic ESPHome-style
integration or a complex flashing framework (both considered and
rejected; see [ARCHITECTURE.md](ARCHITECTURE.md#ota-m12) for the
investigation). M12.4 delivers that. M12.1/M12.2 remain useful,
lower-priority tooling ideas, not blockers -- `idf.py build flash
monitor` and `scripts/ota.sh` already cover simple flashing and Wi-Fi
updates respectively; JTAG/OpenOCD debugging is a nice-to-have this
milestone doesn't require.

### M12.1 — Debugging (optional, not required for v1.0)

- [ ] JTAG/OpenOCD setup.
- [ ] VS Code launch configuration.
- [ ] Debugging documentation.

### M12.2 — Flashing (optional, not required for v1.0)

- [ ] Flash helper script(s) beyond `idf.py flash`/`scripts/ota.sh`.
- [ ] Automatic serial-port detection where practical.

### M12.3 — Build & Release ✅

Firmware metadata only -- no product functionality changed. Goal: any
build a user reports a problem with can be identified exactly from its
own boot log. See
[ARCHITECTURE.md](ARCHITECTURE.md#firmware-metadata-m123) for the full
design.

- [x] `version.txt` at the repository root is now the single source of
      truth for the firmware version string (`v1.0.0-dev`) -- ESP-IDF's
      own build system already reads it into `PROJECT_VER` ahead of
      `git describe`, embedding it into the app image's `esp_app_desc_t`
      (the same structure OTA/rollback would later compare between
      slots). One line, one file; nothing else in the codebase defines a
      version string.
- [x] New `components/firmware_info/` exposes
      `Version()`/`GitCommit()`/`GitBranch()`/`GitDirty()`/
      `BuildDate()`/`BuildTime()`. `Version()`/`BuildDate()`/`BuildTime()`
      wrap ESP-IDF's own `esp_app_get_description()` (zero duplication);
      `GitCommit()`/`GitBranch()`/`GitDirty()` have no ESP-IDF
      equivalent, so a build-time (not just configure-time) CMake target
      regenerates them from `git` on every build, reported as
      `"unknown"`/clean rather than failing the build if `git` or `.git`
      aren't available -- works unchanged in CI, a shallow clone, or a
      source archive, not just one developer's machine.
- [x] `app::App::Run()` logs a concise, four-line boot block (project
      name, firmware version, commit/branch/dirty, build date/time)
      before any other subsystem initializes.
- [x] Verified: clean `idf.py build`; every value read from
      `version.txt`/`git`/ESP-IDF's own descriptor, no hardcoded
      placeholder anywhere; a new commit changes the reported commit on
      the very next build with no reconfigure needed; no duplicate
      version string exists anywhere in the project. Boot log format not
      verified on physical hardware from this environment (no serial
      monitor reachable here), but the exact same string is what
      `idf.py monitor` would show, built from the same values already
      confirmed correct via the generated header/build log.

### M12.4 — OTA ✅

A first OTA mechanism (`esp_https_ota`/TLS) was built, hardware-tested,
and fully reverted after failing on real hardware with a heap-
fragmentation-driven mbedTLS allocation failure -- see git history and
[ARCHITECTURE.md](ARCHITECTURE.md#ota-m12) for the root cause. This
replacement deliberately avoids TLS entirely rather than working around
that failure: a small `esp_http_server`-based endpoint, plain HTTP,
shared-secret authentication, streamed straight into `esp_ota_write()`
with no full-image buffering. See
[ARCHITECTURE.md](ARCHITECTURE.md#ota-m12) for the full design
(transport, flow, partitions, rollback, security posture, and the
`scripts/ota.sh` developer workflow).

- [x] New `components/ota/` (`ota::OtaServer`): `GET /version`
      (unauthenticated, read-only) and `POST /ota` (requires an
      `X-OTA-Token` header, constant-time compared against a shared
      secret) -- reuses `esp_ota_get_next_update_partition()`/
      `esp_ota_begin()`/`esp_ota_write()`/`esp_ota_end()`/
      `esp_ota_set_boot_partition()` directly, no custom OTA protocol.
      Never buffers the full image (4 KiB streaming chunks straight from
      the socket into flash).
- [x] Shared secret via `ota::OtaSecretProvider` -- gitignored
      `ota_secret_local.hpp`, mirroring `grohe_ble`/`time_service`'s
      existing local-credentials pattern exactly. Empty secret disables
      the OTA endpoint entirely rather than ever accepting an
      unauthenticated upload.
- [x] `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` reinstated
      (`sdkconfig.defaults`). `ota::ConfirmBootValid()` is called from
      `App::Run()` only after display/UI/encoder/BLE have all finished
      initializing -- a genuine improvement over the earlier M12.4
      design, which confirmed too early to catch a startup-time crash.
- [x] `OtaServer` is a second, permanent `WifiConnection` consumer
      (`AcquireAsync()`d once, never released) -- Wi-Fi stays connected
      for the whole process lifetime so the endpoint is reachable on
      demand, not just briefly after boot. Still fully non-blocking and
      non-fatal: every other subsystem starts up exactly as before
      regardless of Wi-Fi's state.
- [x] `scripts/ota.sh <device-ip>`: builds the firmware itself
      (`idf.py build`, aborting immediately on any build failure -- no
      OTA attempt with a stale/missing binary), prints the version/
      commit/branch actually embedded in the just-built binary *before*
      upload, uploads with `curl` (unchanged OTA protocol), polls
      `/version` after the reboot, and reports `OTA SUCCESS` only if the
      version *and* commit the device reports afterward match what was
      just built -- never a blind `version.txt` comparison.
- [x] Reused the OTA-ready partition table from M9 unchanged (`ota_0`/
      `ota_1`/`otadata`) and M12.3's firmware version metadata
      (`firmware_info::Version()`/`GitCommit()`, read by `/version`).
- [x] Verified: clean `idf.py build` (including with the rollback
      Kconfig applied); scope diff confirms zero BLE/Grohe-protocol/
      dispense/DialController/UI/display-rotation files touched.
- [x] Verified on hardware: USB-flashed as the initial install; Wi-Fi
      connects and the OTA endpoint comes up; a full `scripts/ota.sh`
      Wi-Fi OTA update completed and the device rebooted into the new
      image, `/version` confirmed before and after; USB flashing still
      works afterward as recovery. Display (upright, `k0` -- see
      [board_config.hpp](../components/board/include/board/board_config.hpp)),
      encoder, button, the BLE connection to the Grohe Blue, and Still/
      Medium/Sparkling dispensing plus stop/cancel all confirmed working
      post-update, with no regressions.
- [x] Hardware bring-up also caught and fixed a real bug in
      `time_service::WifiConnection`, not specific to OTA itself:
      `AcquireAsync()` tracked only one pending caller at a time, so a
      second concurrent consumer's callback was silently dropped rather
      than ever invoked. `OtaServer` becoming Wi-Fi's first consumer this
      milestone made that reachable for the first time --
      `SntpTimeProvider`'s callback was the one being lost, so SNTP time
      sync never completed even though Wi-Fi itself connected correctly
      (the dial stayed on "Synchronisierung..."). Fixed with a proper
      multi-waiter queue guarded by a mutex that's always released before
      any callback runs (no lock held across arbitrary caller code, no
      re-entrancy/deadlock risk) -- see
      [ARCHITECTURE.md](ARCHITECTURE.md#wifi-connectivity). Time sync
      confirmed completing on hardware after the fix.
- [x] Verified on hardware a second time, end to end, using
      `scripts/ota.sh`'s automatic-build workflow: a single
      `./scripts/ota.sh <device-ip>` run built the firmware, uploaded it,
      and confirmed the device rebooted into exactly that build --

      ```
      Device:   192.168.178.64
      Firmware: build/grohe_dial.bin
      Upload accepted (HTTP 200) -- device is rebooting.
      Device is back up.

      === Firmware running on device ===
      Version: v1.0.1-dev
      Commit:  0a3bb63
      Branch:  main (dirty)
      ==================================

      OTA SUCCESS -- device is running the firmware just built and flashed.
      ```

      Not part of this run, and not required for M12: rollback behavior
      from a deliberately unconfirmed/failing boot -- the mechanism is
      implemented and Kconfig-enabled (see
      [ARCHITECTURE.md](ARCHITECTURE.md#ota-m12)'s "Rollback" design) but
      that specific scenario remains hardware-untested.

## M13 — Home Assistant Integration ✅ (goals achieved via M15, not this milestone's original MQTT design)

Home Assistant extends the product; it never becomes a runtime dependency
of it -- the same "optional, not a dependency" principle M9 already
established for Wi-Fi/SNTP. Local BLE operation always has priority, and
the dial must remain fully usable -- dispensing, stopping, reconnecting,
everything it already does today -- whether or not Home Assistant or
Wi-Fi is present or reachable.

Architecture (see the M13 proposal in project history and
[ARCHITECTURE.md](ARCHITECTURE.md) for the full reasoning): a hybrid of
local HTTP (for one-time credential provisioning, reusing M12's OTA
HTTP-server/auth pattern with its own separate secret) and MQTT + Home
Assistant MQTT Discovery (for ongoing state, settings, and optional
control). No cloud code of any kind enters this firmware -- Home
Assistant's existing `grohe_smarthome` integration already handles Grohe
Cloud login and appliance discovery; this project only ever consumes
that, via a new, minimal `grohe_dial` HA integration, never duplicates
it. Concretely, the dial does **not** publish CO₂, filter level, or the
*appliance's* firmware version -- those already exist as
`grohe_smarthome`-owned entities, sourced from the cloud (a BLE-only
client structurally cannot read them at all; see the M13 proposal's
research). Home Assistant's role here is the dial's *own* state and
settings (availability, the dial's own firmware version, default
amount/encoder step/default water type, optionally live dispense
state/control) -- never appliance diagnostics the existing integration
already owns.

### M13.1 — Persistent settings & runtime credentials ✅

- [x] New `components/settings/` (`settings::DialSettingsStore`): NVS-
      backed default dispense amount, encoder step size, and default
      water type -- overrides on top of `dial_state.hpp`'s own
      compile-time constants, which remain the fallback for a device
      with nothing stored. Validates before persisting (in-range
      amount/step, a real `WaterType`) and again on load, so a
      corrupt/out-of-range stored entry falls back to the compile-time
      defaults rather than producing broken behavior. Independent of
      `app/` and everything above `dial_state/` in the dependency graph.
- [x] `app::DialController::ApplySettings()`: applies loaded settings
      once, at startup, before the first UI render -- the encoder's
      rotate step (`HandleEvent()`'s `kRotateCw`/`kRotateCcw` cases) now
      reads a member overridden by this call instead of
      `dial_state::kAmountStepMl` directly. Skipping the call entirely
      leaves behavior identical to before this milestone.
- [x] New `grohe_ble::NvsCredentialsProvider` (`grohe_credentials.hpp`/
      `nvs_credentials_provider.cpp`), alongside the existing
      `LocalCredentialsProvider`, not replacing it: reads/writes a
      provisioned `{user_id, preshared_key}` pair from/to NVS as a
      single blob entry (not two separate keys) -- the concrete
      mechanism behind "no partially-written credential set" this
      milestone's own scope calls for, since a torn/interrupted write
      is caught by NVS's own per-entry CRC on the next read rather than
      ever surfacing as a mix of old and new values. Falls back to an
      owned `LocalCredentialsProvider` whenever nothing is provisioned
      yet or a stored entry fails validation -- provisioning is strictly
      additive to the existing developer workflow. Never logs either
      field's value, only lengths/success/failure.
- [x] `grohe_ble::GroheClient` now takes its `CredentialsProvider` by
      constructor injection (`const CredentialsProvider&`) instead of
      hardcoding a `LocalCredentialsProvider` member -- the same
      dependency-injection shape already used for `WifiConnection`/
      `OtaSecretProvider`. `app::App` (the composition root) decides
      which concrete provider a given build actually uses.
- [x] No MQTT, no Home Assistant integration, no local HTTP endpoint yet
      -- both land in later M13 sub-milestones. Nothing in BLE protocol
      behavior, Wi-Fi, OTA, display orientation, or UI changed beyond
      `DialController` reading settings through the new seam described
      above.
- [x] Verified: clean `idf.py build`, zero new warnings from any
      touched/new file; scope diff confirms only `components/settings/`
      (new), `components/grohe_ble/` (credentials provider + DI), and
      `components/app/` (wiring the two into the composition root) were
      touched -- no BLE protocol, Wi-Fi, OTA, or UI/display files.
- [x] Verified on hardware: persisted settings survive a reboot, and
      runtime-provisioned credentials (via M13.2's `/provision`) were
      successfully stored, picked up by `NvsCredentialsProvider` without
      a reboot, and used end to end for a real BLE dispense.

### M13.2 — Local provisioning endpoint ✅

See [ARCHITECTURE.md](ARCHITECTURE.md#provisioning-m132) for the full
design (why a separate `esp_http_server` instance/port from OTA's,
request/response format, credential-activation behavior, and the
complete security posture).

- [x] `POST /provision` (new `components/provisioning/`,
      `provisioning::ProvisioningServer`) -- its own `esp_http_server`
      instance on its own port (`8080`; OTA already owns port 80 and
      `components/ota/` is untouched by this milestone), reusing OTA's
      shared-secret-header/constant-time-compare/fail-closed *pattern*
      with its own separate secret (`X-Provision-Token`, gitignored
      `provisioning_secret_local.hpp`) -- never the OTA token.
      Permanently reachable whenever Wi-Fi is connected: no button/
      gesture gating, no temporary provisioning window (this dial has
      exactly one physical button, already assigned to dispensing/stop
      and the water-type cycle -- M13.2 adds no new interaction to it).
- [x] Request: `{"user_id": "...", "preshared_key_base64": "..."}`,
      parsed with cJSON (ESP-IDF's bundled `json` component). Validates
      the complete request (auth header, JSON well-formedness, field
      presence/type/non-empty/length against
      `grohe_ble::kMaxCredentialFieldLen`) *before* ever calling
      `NvsCredentialsProvider::Set()`, which itself writes the whole
      `{user_id, preshared_key}` pair as one atomic NVS blob (M13.1's
      own mechanism, reused here rather than inventing a second
      credential store) -- an invalid request touches no NVS state at
      all, and a request that fails inside `Set()` leaves whatever was
      already stored untouched. `200 OK` only after a successful commit.
- [x] Status codes: `200` (stored), `400` (malformed JSON or missing/
      invalid fields), `401` (missing or wrong `X-Provision-Token`),
      `500` (valid request, NVS-level failure). No credential value ever
      appears in a response body, a log line, or an error message.
- [x] Credential activation: no reboot required. `NvsCredentialsProvider
      ::Set()` already updates its own in-memory cache immediately (a
      genuine M13.1 property, not new complexity added for this
      milestone), and `GroheClient` reads credentials fresh on every
      command it sends -- the next dispense/stop after a successful
      `/provision` call uses the new credentials automatically. The
      response body reports `"reboot_required": false` explicitly.
- [x] `ProvisioningServer` wired into `app::App` as `WifiConnection`'s
      third consumer (after `SntpTimeProvider`, `OtaServer`), same
      non-blocking/non-fatal shape as the other two -- Wi-Fi/BLE/
      dispensing/UI/startup never depend on it. Without any provisioned
      credentials, `grohe_ble::LocalCredentialsProvider` continues to
      work exactly as before M13 started.
- [x] Verified: clean `idf.py build`, zero new warnings from any
      touched/new file; the JSON request-validation logic specifically
      re-verified in a standalone host-side harness (cJSON is portable
      C, compiled outside ESP-IDF for this) against nine cases --
      valid request, malformed JSON, missing `user_id`, missing
      `preshared_key_base64`, empty `user_id`, non-string `user_id`,
      empty body, a non-object body, and a 200-byte over-length field --
      all nine matched their expected accept/reject outcome. Scope diff
      confirms `components/ota/` and `scripts/ota.sh` are untouched, and
      no BLE protocol, Wi-Fi, UI, or display file was touched beyond the
      M13.1 seams this milestone reuses.
- [x] Verified on hardware: the provisioning server is reachable on its
      own port; an unauthorized request (missing/wrong `X-Provision-Token`)
      returns `401`; a malformed/incomplete JSON body returns `400`; a
      valid request returns `200 OK` with `reboot_required: false` and
      the new credentials are usable immediately, with no reboot, exactly
      as designed. `curl` commands used for that pass (`<token>` is
      whatever `provisioning_secret_local.hpp` was filled in with):

      ```sh
      # Missing token -> 401
      curl -i -X POST http://<device-ip>:8080/provision \
        -H "Content-Type: application/json" \
        -d '{"user_id":"abc","preshared_key_base64":"xyz"}'

      # Wrong token -> 401
      curl -i -X POST http://<device-ip>:8080/provision \
        -H "X-Provision-Token: wrong-token" \
        -H "Content-Type: application/json" \
        -d '{"user_id":"abc","preshared_key_base64":"xyz"}'

      # Malformed JSON -> 400
      curl -i -X POST http://<device-ip>:8080/provision \
        -H "X-Provision-Token: <token>" \
        -H "Content-Type: application/json" \
        -d '{"user_id": "abc", '

      # Missing field -> 400
      curl -i -X POST http://<device-ip>:8080/provision \
        -H "X-Provision-Token: <token>" \
        -H "Content-Type: application/json" \
        -d '{"user_id":"abc"}'

      # Valid request -> 200
      curl -i -X POST http://<device-ip>:8080/provision \
        -H "X-Provision-Token: <token>" \
        -H "Content-Type: application/json" \
        -d '{"user_id":"abc-123","preshared_key_base64":"c29tZWJhc2U2NA=="}'
      ```

### M13.3 — MQTT client & Home Assistant Discovery (implemented, then removed in M15)

Implemented (MQTT client + HA Discovery for the dial's own settings and
BLE connection status), hardware-verified working, then **fully removed
in M15** in favor of a native Home Assistant integration over local HTTP
-- see [`docs/m15_ha_integration.md`](m15_ha_integration.md). No MQTT
code remains in the tree; `docs/mqtt_ha_discovery_plan.md` (this
milestone's own design doc) was deleted alongside it. Kept here only as
a historical record that this milestone happened and worked before being
superseded, not as a currently-accurate description of the firmware.

### M13.4 — `grohe_dial` Home Assistant integration (superseded by M15)

Never implemented as originally scoped (MQTT-Discovery-based, no polling
coordinator). Superseded by M15's native HTTP-based integration, which
covers the same goal (a real `grohe_dial` HA integration) with a
different, non-MQTT transport -- see
[`docs/m15_ha_integration.md`](m15_ha_integration.md).

### M13.5 — Optional dispense/stop/live-state over MQTT (superseded by M15)

Never implemented. Superseded by M15, which covers dispense/stop/live
state over local HTTP instead of MQTT from the start.

### M13.6 — Dispense failure feedback (implemented, hardware-verified)

Found during M13.1/M13.2 hardware validation: `/provision` was used to
store deliberately invalid test credentials, provisioning itself
succeeded (`200 OK`), and the subsequent dispense attempt then failed
exactly as the BLE protocol says it should (`INVALID_HMAC`) -- but the
display showed nothing at all. No water ran, no text appeared, the ring
didn't change, and the connection dot stayed solid blue throughout. For
a standalone device with no companion app and no serial monitor, that's
a real gap: the user has no way to tell "still working" from "already
failed."

See [`docs/ui/error_feedback_concepts.md`](ui/error_feedback_concepts.md)
for the full design study (current-architecture analysis, four UI
concepts evaluated against the frozen dispense-UI spec's own house
rules, a recommendation, and a proposed error classification) and its
companion mockup artifact (linked from that document) for the visual
storyboard at real 240 px proportions.

- [x] **Design study complete**: root cause traced to
      `DialController::HandleCommandOutcome()` discarding a rejected
      dispense's outcome (logged, never reaches `DialState`) -- a side
      effect of M11.1's deliberate removal of raw protocol-code display,
      not a new decision. Four concepts evaluated (temporary overlay,
      dedicated error screen, inline text only, a transient fail glyph
      reusing the existing `Finished` checkmark mechanism); the last is
      recommended -- zero new LVGL widgets/screens/navigation, reads at
      a glance, and is structurally distinct from a real BLE disconnect
      (a one-shot ~1.5 s pulse vs. a sustained state) rather than reusing
      its exact visual signature. Confirmed via `dial_state.hpp`'s own
      existing "never conflate independent subsystems" principle
      (already applied to `ConnectionStatus`/`TimeStatus`) that the BLE
      connection dot should stay connection-only, never mixed with
      command-result -- matching the direction already leaned toward
      before this study.
- [x] **Implemented**: Concept D (transient fail glyph), exactly as
      recommended. `dial_state::DispenseStatus` gained one new
      enumerator, `kFailed`, entered directly from `kIdle` by
      `DialController::HandleCommandOutcome()` on a rejected dispense
      request (`outcome.was_dispense && dispense_status == kIdle`); a
      rejected *stop* is unaffected and still reverts to `kDispensing`
      as before. Mirrors the existing `kFinished` mechanism exactly: a
      fixed ~1.5 s hold (`DialController::kFailedHoldUs`) tracked the
      same way as `finished_until_us_`, read by the same `Tick()`, no
      button press required to leave it. `UiManager` reuses the
      checkmark's own widget slot (renamed `status_glyph_label_` since
      it now serves two symbols) showing `LV_SYMBOL_CLOSE` instead of
      `LV_SYMBOL_OK`; `hint_label_` shows "Bad credentials" (see below
      for why this replaced the original "Try again"); the ring
      desaturates for the hold's duration via the *existing*
      `ready`/`last_ring_ready_` guard (no new `lv_anim_t`). No new
      widgets, no new navigation, no permanent error screen, no red/
      blinking treatment. The BLE connection dot is untouched by any of
      this -- `connection_status` has no pathway from
      `HandleCommandOutcome()`, so "BLE connected + command rejected"
      stays representable exactly as the design study required.
      `idf.py build` is clean (no new compiler warnings on the touched
      files). `LV_SYMBOL_CLOSE`'s availability, flagged as unverified in
      the design study, is now confirmed **at compile time** (same
      symbol font as the already-working `LV_SYMBOL_OK`) -- see the next
      bullet for what's actually verified on-device.
- [x] **Hardware-verified**: wrong-credentials dispense request tested
      end-to-end on real hardware (`/provision` given deliberately
      invalid `user_id`/`preshared_key_base64`, then a dispense
      attempted) -- the glyph appears immediately, no water runs, ring
      desaturation is visible but subtle, auto-return to Ready lands
      cleanly at ~1.5 s, and the connection dot stays solid blue
      throughout since the BLE link itself never drops. The `hint_label_`
      copy was changed from the originally-tested "Try again" to the
      more specific "Bad credentials" *after* this hardware run, purely
      as a `lv_label_set_text()` string swap on the same already-verified
      code path (no change to timing, layout, glyph, or the ring) --
      still a generic `kFailed` message, not a real per-`response_code`
      classification (see `docs/ui/error_feedback_concepts.md` §5). Text
      width was checked against the existing, already-shipping
      "Connection lost" hint (same 16-character length, similar-or-wider
      per the font's own character widths) rather than re-measured on
      hardware; `hint_label_` has no fixed width/wrap configured, so it
      cannot truncate text at the widget level either way.

### M14 — RAM & Heap Optimization ✅

Follow-up to the M13.3 hardware investigation: MQTT frequently failed to
connect or crashed with an uncaught `std::bad_alloc` while publishing HA
Discovery configs. A systematic RAM/heap forensics pass (per-checkpoint
`[MEM]` logging via the new `components/mem_diag/` helper, kept in the
tree as permanent lightweight diagnostics) traced this to the ESP32-C3's
internal heap simply being too tight by the time BLE + Wi-Fi + MQTT +
OTA + Provisioning are all resident at once -- not a network or protocol
bug. This milestone is the resulting, individually A/B-tested RAM
recovery effort; see `sdkconfig.defaults`'s own per-candidate comments
for the full reasoning, measured numbers, and evidence behind each one.

- [x] **OTA/httpd task stack**: `6144 → 4096` B
      (`components/ota/ota_server.cpp`). Real measured peak usage
      ~612-2012 B; no OTA/httpd-specific fault across a 4-run A/B series.
      MQTT's own task stack was tested at the same reduction and
      **explicitly reverted** -- 4096 B reproducibly prevented
      `MQTT_EVENT_CONNECTED` from ever being reached (0/5 runs), while
      6144 B connected in 7/7 boot cycles across 2 runs. **MQTT task
      stack intentionally stays at its library default, 6144 B.**
- [x] **NimBLE roles**: Peripheral/Broadcaster/Observer disabled, Central
      kept -- this dial only ever scans and connects as a GATT client,
      never advertises (`ble_gap_adv_start`/`ble_gatts_*`: zero call
      sites in `components/grohe_ble/`).
- [x] **BLE scan/filter buffers reduced** for a single-target,
      stop-on-first-match scan (`ble_gap_disc_cancel()` in
      `BleManager::OnDeviceFound()`): advertising-report flow-control
      queue, scan duplicate-address cache, and the (unused, no extended
      advertising) BLE 5.0 duplicate filter.
- [x] **`BT_CTRL_BLE_MAX_ACT`**: `6 → 2` controller activity instances
      (one scan, one connection -- this firmware never advertises or
      uses periodic-adv sync). ~3.3 KB.
- [x] **NimBLE MSYS/ACL/HCI event buffer pools** resized for a single
      low-throughput peripheral connection instead of the library's
      busy-multi-connection default. **~13.6 KB -- the single largest
      contributor**, independently confirmed against the Kconfig's own
      documented per-buffer costs.
- [x] **`BT_NIMBLE_MAX_CCCDS`**: `8 → 1` (this firmware subscribes to
      exactly one notification characteristic).
- [x] **~20 KB internal RAM recovered** in total (measured consistently
      across `BLE_INITIALIZED`/`MQTT_STARTED`/`PROVISIONING_INIT`
      checkpoints); `BLE_PRE_INIT → BLE_INITIALIZED` (NimBLE's own
      init-time cost) fell from ~64.4 KB to ~46.2 KB.
- [x] **The `std::bad_alloc` HA Discovery crash did not reproduce** in
      any of ~13 post-optimization hardware test runs in this
      investigation (5 dedicated Discovery-publish stress boots, a
      5-minute continuous-operation soak test, and several others) --
      `MQTT_DISCOVERY_DONE` now consistently lands with several KB of
      free internal heap and a multi-KB largest contiguous block, versus
      low-hundreds-of-bytes/a sub-1.5 KB largest block beforehand.
      Reported as "no longer reproducible under test," not as a formal
      proof the underlying race is structurally impossible.
- [x] **Unplanned side effect**: the pre-existing Provisioning
      `httpd_start()` failure (`ESP_ERR_HTTPD_TASK`, present since the
      M13.3 MQTT-before-Provisioning startup reorder) did not reproduce
      in any of the 11 test runs since the NimBLE buffer-pool reduction
      landed -- plausibly explained by the extra headroom, not
      independently fixed.
- [x] **Automated hardware validation**: full BLE chain (found, connect,
      GATT discovery, all characteristics, notification subscribe), 5
      consecutive clean Discovery-publish boot cycles, a 5-minute
      continuous-operation soak test (0 crashes/reboots/malloc
      failures/watchdog events), OTA `GET /version` + endpoint routing,
      and a live MQTT command → state round-trip via the broker -- all
      passing on real hardware with the fully optimized configuration.
- [x] **Manual hardware validation, real Grohe Blue Dial** (2026-09-11):
      with this milestone's fully optimized firmware flashed, the
      complete water-dispensing path was exercised directly on the
      device -- Still, Medium, and Sparkling/Carbonated water each
      dispensed successfully; different pour amounts via the rotary
      encoder worked correctly; Stop/Cancel mid-dispense worked and BLE
      stayed connected afterward; BLE connect and HMAC command
      authentication both functioned throughout. This is a manual
      physical test, not something any automated boot-log capture in
      this investigation triggered or could trigger (no debug/test hook
      exists to simulate encoder input or a dispense command from this
      environment) -- recorded here because it closes the one gap the
      automated validation above could not cover on its own.
- [x] **Since exercised for real** (post-M15, and repeatedly during M16):
      a real `POST /ota` upload through `esp_ota_write()`'s actual
      flash-write path at this milestone's 4096 B httpd stack size, on
      the real device -- no fault at that stack size specifically.
      M16 separately found that OTA can become unreliable when the
      device is stuck rebooting during its own boot window (BLE/Wi-Fi
      radio contention on the shared ESP32-C3 radio, not a stack-size or
      flash-write defect) -- see
      [`docs/m16_reliability_and_provisioning.md`](m16_reliability_and_provisioning.md)
      §7. USB remains the unconditional fallback for that specific case.

### M15 — Native Home Assistant Integration (Local HTTP, MQTT Removed) ✅

Replaces M13.3/M13.4/M13.5's MQTT-based approach entirely (see those
sections' own "superseded by M15" notes above) with a local HTTP API on
the dial plus a real, native Home Assistant custom integration -- no
MQTT, no cloud dependency. Full detail, evidence, and the exact test/RAM
numbers live in
[`docs/m15_ha_integration.md`](m15_ha_integration.md); this entry is
the roadmap-level summary.

```
Home Assistant -> Grohe Dial HA Integration -> local HTTP -> Grohe Dial -> BLE -> Grohe Blue Home
```

- [x] **MQTT removed in full**: `components/dial_mqtt/` deleted, every
      reference cleaned up repo-wide (verified by grep -- only
      historical/dated commentary remains).
- [x] **Local HTTP API** on the existing Provisioning httpd instance
      (port 8080, no new task): `GET /api/status`, `GET`/`POST
      /api/config`, `POST /api/dispense`, `POST /api/stop`, all gated by
      a dedicated `X-Api-Token` (separate from the provisioning/OTA
      tokens). Reuses the encoder's own dispense/stop call chain
      unmodified via a cross-task queue hand-off
      (`components/dial_api/`), the same pattern `BleManager`'s own
      command queue already established.
- [x] **Native Home Assistant custom integration**
      (`homeassistant/custom_components/grohe_dial/`) -- a real Config
      Entry (host/port/API token, local-only, no cloud login), device +
      9 entities across 5 platforms, two services
      (`grohe_dial.dispense`/`grohe_dial.stop`). Cloud login /
      `ha-grohe_smarthome` linking deliberately deferred (see the doc's
      own §4.4) -- a dedicated architecture review recommended against
      forking that project (cloud-only device model, near-zero code
      reuse), not an M15 blocker.
- [x] **Automated tests**: 37 passed (grown from an initial 14 as real
      gaps were found during hardware acceptance) -- HTTP API client,
      config flow, full entity setup, simulated end-to-end dispense/stop
      flows, and an executable encoding of the dial's own state-field
      consistency contract.
- [x] **Hardware acceptance, including the real Home Assistant
      integration**: installed on a real HA instance, Config Flow
      completed against the real dial, Still/Medium/Sparkling each
      dispensed for real via `grohe_dial.dispense`, a 1340 mL pour
      stopped mid-flight via `grohe_dial.stop` (confirmed by the
      firmware's own serial log), config round-trip verified via a
      direct API read (not just the HA-side cached value), 40 rapid
      parallel requests with 0 errors, 0 crashes across the whole pass.
- [x] **Two real bugs found and fixed during hardware acceptance**
      (`docs/m15_ha_integration.md` §6a): a `NumberSelector`-sourced
      float port silently broke the Config Flow's persisted data
      (`141db73`); both HA services were registered as plain lambdas,
      which Home Assistant's dispatcher never awaited, so service calls
      reported success while doing nothing at all (`57d2b7e`). Both
      have dedicated regression tests that fail against the old code and
      pass against the fix.
- [x] **Concurrency fix**: an independent review of `App::Status()`
      found it could, in principle, read `DialController::State()`
      mid-transition from the wrong task, risking an internally
      inconsistent snapshot (not just a stale one). Closed by routing
      the read through the same app-task queue the write side already
      uses (`d8ff91c`).
- [x] **Since tested for real**: a real `POST /ota` firmware upload
      (ad-hoc, right after this milestone closed, rebuilding `main`) and
      repeatedly again during M16's own interactive deployment steps --
      see M14's own updated entry above for the one caveat M16 found
      (boot-time radio contention, not this upload path itself).
- [ ] **Still deferred, not a blocker**: `via_device` device linking, the
      dial's stable MAC-based `unique_id` (host/IP used instead today),
      multi-appliance BLE disambiguation. Cloud login itself was added in
      M16 (`homeassistant/custom_components/grohe_dial/cloud.py`), but
      only for one-shot BLE-credential *provisioning* -- not the ongoing
      Cloud-linked device model this bullet originally meant, which
      remains undone. CO₂/filter/consumables are a hard architectural
      boundary, not a deferred feature -- the dial's BLE link to the
      Grohe Blue Home never carries that data.

Committed as four commits, all merged to `main` (`bb10480`, `d8ff91c`,
`141db73`, `57d2b7e`), M14 (`c31058a`) unchanged as their ancestor.

### M16 — Reliability Hardening + Grohe Blue Provisioning ✅

Two tracks: hardening the M15 HTTP-API/Home-Assistant boundary
(transient-failure retry, actionable errors, an app-task hang
safety-net), and a new Home Assistant Options Flow that provisions a
dial's Grohe Blue Home BLE credentials via the existing `grohe` PyPI
package/`scripts/grohe_cloud_*.py` reference implementation --
**zero firmware changes**, the existing `POST /provision` endpoint
(M13.2) already accepted exactly what that package produces. Full
detail, every status tag, and the exact test/hardware evidence live in
[`docs/m16_reliability_and_provisioning.md`](m16_reliability_and_provisioning.md);
this entry is the roadmap-level summary. All work on branch `m16`;
`main` untouched at `96f4b16` until every item below reached PASS.

- [x] **Connection retry/backoff** (M16.1): `DataUpdateCoordinator`'s
      own native `retry_after` mechanism, 5s/10s/20s/30s(capped)
      exponential backoff on transient connection failures only, reset
      on recovery. 7 tests. Hardware-verified, including a real ~3.15s
      outage across a real device reset (M16.5's own test doubles as
      re-confirmation). (`c601d70`)
- [x] **Actionable error messages** (M16.2): dispense/stop failures now
      raise `HomeAssistantError` with a specific `translation_key`
      (`dial_unreachable`/`command_rejected`/`unexpected_dial_error`)
      instead of a generic exception string. 6 tests. Hardware-verified.
      (`595f5c5`)
- [x] **App-task watchdog** (M16.3): TWDT registration +
      `CONFIG_ESP_TASK_WDT_PANIC=y` -- a future app-task hang now
      triggers an automatic panic-reset (composes correctly with the
      existing OTA rollback guarantee) instead of freezing the dial
      forever. Mechanism hardware-proven twice: once via a temporary
      diagnostic build in an earlier session, and again this session via
      a temporary, isolated hang test built directly on top of the
      *clean, current* M16.3 source (`3ba1828`) and verified over 4
      consecutive cycles via USB -- reverted immediately after capture.
      The physical device now runs the clean production build. (`f16ccbe`)
- [x] **BLE disconnect during a real dispense** (M16.4): a temporary,
      controlled test hook forced a real `ble_gap_terminate()` ~1.5s into
      a genuine, minimal (100ml) dispense against the real Grohe Blue
      Home. Full automatic recovery in ~2s, zero crashes, consistent
      post-recovery state. No bug found -- existing M8/M11.1 logic
      already handled it correctly; now hardware-confirmed, not just
      unit-level.
- [x] **Boot-time HA/BLE race** (M16.5): an aggressive HTTP polling loop
      run continuously across a real device reset found a clean ~3.15s
      connection-refused window (matching `httpd_start()` timing exactly)
      and zero corruption/crashes either side of it -- well inside
      M16.1's own 5s initial backoff. No bug found.
- [x] **Grohe Cloud provisioning** (M16.6-M16.9): new Options Flow
      (Cloud login -> appliance selection -> dial provisioning token ->
      `POST /provision`), reusing the existing `grohe` package
      end-to-end -- no Grohe Cloud API logic reimplemented anywhere in
      this integration. 21 new tests (cloud auth/discovery, the full
      flow including error paths, idempotent re-provisioning, a
      dedicated no-secrets-in-logs check). 71/71 tests pass across
      `homeassistant/tests/`. (`844da3e`, `afb150f`)
- [x] **Real hardware provisioning** (M16.10): the real pipeline --
      Cloud token refresh, JWT decode, Cloud device discovery, and the
      dial's own `/provision` endpoint -- run end-to-end against the
      real Grohe Cloud (via an existing, genuinely-obtained refresh
      token, no password re-entry needed) and the real physical dial.
      Verified across two reboots (credentials persisted in NVS,
      reconnected to the same real appliance), a real 100ml dispense,
      and a real dispense-then-stop (130/200ml delivered before
      stopping cleanly).

A physical-device reboot loop was found at the start of this milestone's
final hardware-acceptance session -- root-caused (not a new bug: the
device was still running an old, never-committed M16.3 diagnostic build
left over from an earlier session's OTA-recovery difficulties) and
recovered via USB, which this session's own instructions explicitly
made available. See the doc's own §7 for the full account.

## v1.0 Release Criteria

What "version 1.0" means for this project -- the minimum bar for the
first production release, not a milestone in itself. Updated after M16
to reflect what's actually been verified, not just planned; each item
below links to the milestone(s) that are its evidence.

- [x] M10 completed.
- [x] M12 completed. Core (M12.3 Build & Release, M12.4 OTA) is done;
      M12.1 (JTAG/OpenOCD debugging) and M12.2 (flash helper tooling
      beyond `idf.py flash`/`scripts/ota.sh`) remain explicitly optional
      and undone -- see their own headings above. Not a blocker: neither
      was ever load-bearing for shipping, only for developer convenience.
- [x] M13 completed -- via M15, not this milestone's original MQTT
      design (see M13's own updated heading above). Every underlying
      goal (persistent settings, provisioning, a real HA integration,
      dispense-failure feedback) shipped, just over local HTTP instead of
      MQTT + Discovery.
- [x] Stable hardware validation. Every milestone from M0 through M16 has
      its own hardware-verified section above; M16 additionally added
      real forced-failure testing (a real mid-dispense BLE disconnect, an
      aggressive poll loop across a real reboot, real Grohe Cloud
      provisioning) on top of the happy-path validation earlier
      milestones already had.
- [x] Reliable flashing workflow. USB (`idf.py flash`) has never failed
      across the whole project. OTA (`scripts/ota.sh`) is reliable in
      normal operation (used repeatedly through M15 and M16's own
      interactive deployment steps) with one known, narrow exception: a
      device stuck rebooting during its own boot window can outrun OTA's
      upload window due to BLE/Wi-Fi radio contention -- see M14's and
      M15's updated OTA bullets above and
      [`docs/m16_reliability_and_provisioning.md`](m16_reliability_and_provisioning.md)
      §7. USB is the unconditional fallback for that one case, and M16.3
      now also makes a hang that *causes* such a loop self-correct via
      the watchdog rather than requiring recovery at all.
- [x] Reliable debugging. Not via M12.1's originally-planned JTAG/OpenOCD
      setup (still undone, still optional) -- via serial log capture and
      systematic root-causing instead, the same method used successfully
      across every hardware investigation in this project, including
      M14's RAM forensics and M16's own reboot-loop diagnosis. Judged
      sufficient in practice, not just in principle.
- [x] No known **critical** defects. Known, non-critical limitations are
      tracked, not hidden: OTA's boot-time-radio-contention edge case
      (above, USB fallback always available), the dial's host/IP-based
      (not MAC-based) `unique_id`, no `via_device` HA linking, no
      multi-appliance BLE disambiguation, and the hard architectural
      boundary that CO₂/filter/consumables data is Cloud-only and never
      reaches this BLE-only firmware. None of these affect core dispense/
      stop/BLE/UI reliability, which M0-M16's hardware acceptance
      consistently found solid.
- [x] Complete documentation. `README.md`, `docs/ARCHITECTURE.md`,
      `docs/ROADMAP.md` (this file), `docs/m15_ha_integration.md`,
      `docs/m16_reliability_and_provisioning.md`, `docs/ui/*`,
      `SECURITY.md`, and `hardware/enclosure/` together cover the
      protocol, firmware architecture, every milestone's own evidence,
      the HA integration, security posture, and physical build --
      updated alongside the code they describe, not after the fact.
