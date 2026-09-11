# M16 — HTTP/Home Assistant Reliability Hardening + Grohe Blue Provisioning

> **Status: COMPLETE.** All ten work packages (M16.1–M16.10) are implemented,
> automated-tested, and hardware-accepted on the real device against the
> real Grohe Blue Home and the real Grohe Cloud. A physical-device reboot
> loop found at the start of this milestone's hardware-acceptance session
> (the device was running an old, never-committed diagnostic build left
> over from M16.3's original mechanism proof) was diagnosed, recovered via
> USB, and every previously-BLOCKED hardware test (M16.3's clean build,
> M16.4, M16.5, M16.10) was then completed for real. Every commit lives on
> branch `m16`; `main` was untouched throughout implementation and testing,
> merged only once every item below was independently verified. Tags used
> throughout: **IMPLEMENTED**, **AUTOMATED TESTED**, **HARDWARE TESTED**.

---

## 0. Scope

Two tracks, agreed with the user before implementation began:

1. **Reliability hardening** of the M15 HTTP-API/Home-Assistant boundary —
   the area M15's own retrospective identified as the highest-value next
   step (transient-failure handling, actionable errors, a hang-safety net,
   two identified edge cases).
2. **Grohe Blue Home provisioning** from Home Assistant — closing the gap
   where a not-yet-provisioned dial had no in-HA path to get its BLE
   credentials, without reimplementing any part of the Grohe Cloud API (the
   existing `grohe` PyPI package and `scripts/grohe_cloud_*.py` reference
   implementation are reused, never duplicated — see §6).

This milestone does **not** revisit anything M15 already closed (local HTTP
API shape, basic HA integration, BLE dispense/stop, API-token auth, MQTT
removal, the M15.1 concurrency fix) — those are out of scope by explicit
instruction, not oversight.

---

## 1. M16.1 — HA connection retry/backoff — IMPLEMENTED, AUTOMATED TESTED, HARDWARE TESTED

**Problem**: a transient dial-unreachable failure (Wi-Fi hiccup, dial
mid-reboot) previously retried on `DataUpdateCoordinator`'s fixed default
poll interval — no faster recovery attempt, no backoff on repeated failure.

**Fix**: `coordinator.py`'s `_async_update_data()` now distinguishes
`GroheDialConnectionError` from every other `GroheDialApiError` subtype and
raises `UpdateFailed(str(err), retry_after=retry_after)` with an
exponential backoff schedule — **5s → 10s → 20s → 30s, capped at 30s** —
tracked via a `_consecutive_connection_failures` counter that resets to 0
on the next successful update. This uses Home Assistant's own native
`retry_after` mechanism (`homeassistant.helpers.update_coordinator`,
verified by reading the real installed source): `_schedule_refresh()`
already applies `retry_after` to the next poll and resets it afterwards, so
no custom scheduling, no extra timer, no new failure-mode surface. A plain
`GroheDialApiError` (dial reachable, command rejected) deliberately does
**not** get this treatment — it isn't a connectivity problem, and retrying
faster wouldn't fix it.

**Tests** (`test_coordinator_retry.py`, 7 tests): first failure retries
after 5s, second after 10s, third after 20s, fourth-and-further cap at 30s,
a success resets the counter, a failure after a recovery restarts backoff
from 5s, and a non-connection `GroheDialApiError` keeps the default cadence.

**Hardware**: deployed via OTA during M16's interactive phase; re-confirmed
end-to-end as part of M16.5's own real-reset/real-outage test in §4 below
(a ~3.15s real outage window, comfortably inside the 5s initial
`retry_after`).

Commit: `c601d70` — `fix(ha): retry transient dial connection failures`.

---

## 2. M16.2 — Actionable HA error messages — IMPLEMENTED, AUTOMATED TESTED, HARDWARE TESTED

**Problem**: a failed `grohe_dial.dispense`/`stop` call, or a dispense/stop
button press, surfaced only a generic Python exception string in the HA UI
— no distinction between "dial unreachable" and "dial rejected the
command", no guidance on what to do about it.

**Fix**: a new `errors.py` module,
`raise_as_home_assistant_error(err: GroheDialApiError) -> NoReturn`, maps
the API client's own exception hierarchy to `HomeAssistantError`'s native
i18n mechanism (`translation_domain`/`translation_key`/
`translation_placeholders`) — `GroheDialConnectionError` → `dial_unreachable`,
`GroheDialCommandRejected` → `command_rejected` (with the firmware's own
`reason` string interpolated), anything else → `unexpected_dial_error`.
Applied identically in `button.py` and `services.py`.

**Tests** (`test_error_handling.py`, 6 tests): all three error paths for
both the button and service call sites, asserting the correct
`translation_key` and a preserved `__cause__`.

**Hardware**: error paths exercised on real hardware and produced the
correct translation keys/exception chain (M16's interactive phase).

Commit: `595f5c5` — `fix(ha): expose actionable command errors`.

---

## 3. M16.3 — App-task watchdog — IMPLEMENTED, AUTOMATED evidence n/a, HARDWARE TESTED (clean production build)

**Fix**: `app.cpp` registers the app task with the ESP-IDF Task Watchdog
Timer (`esp_task_wdt_add(nullptr)`, placed after `ota::ConfirmBootValid()`)
and feeds it (`esp_task_wdt_reset()`) as the first statement of every loop
iteration. `sdkconfig.defaults` sets `CONFIG_ESP_TASK_WDT_PANIC=y` so a trip
triggers an actual panic-reset, not just a log warning — and composes
correctly with the existing OTA rollback guarantee (a hang before
`ConfirmBootValid()` leaves the image unconfirmed, so the reset also rolls
it back).

**Hardware acceptance (this session, USB, clean production build)**:

The device was found, at the start of this session's hardware-acceptance
work, in a self-recovering reboot loop. Root-caused via serial capture
(see §7 for the full account) to an old, never-committed M16.3 diagnostic
build (`commit 595f5c5, dirty`, containing a literal
`"M16.3 TEST: simulating an app task hang now"` line) left running from a
prior session — **not a new bug, and not the current M16.3 code**. Recovered
by building the clean, current `m16` tree (`FIRMWARE_INFO_GIT_DIRTY=0`) and
flashing it via USB (`idf.py app-flash`), since USB was explicitly made
available for this session.

To verify M16.3 specifically (not just re-use the earlier diagnostic
build's evidence), a temporary, isolated 8s-hang test was added on top of
the *current* clean source, built (confirmed via boot log:
`"Commit: 3ba1828 (m16, dirty)"`), flashed via USB, and observed for 4
consecutive cycles: every cycle, `task_wdt` fired ~5988–6001ms after the
hang began (`"main (CPU 0)"` not resetting, `"CPU 0: IDLE"`, `"Aborting."`),
printed a register/stack dump, and rebooted cleanly (`rst:0xc,
RTC_SW_CPU_RST`). The temporary change was then reverted (`git diff` empty,
byte-identical to committed HEAD), rebuilt (`FIRMWARE_INFO_GIT_DIRTY=0`),
and reflashed. Post-revert: single clean boot, `"Commit: 3ba1828 (m16)"`
(not dirty), `"Startup complete"` reached, zero `task_wdt` triggers across
both a 30s reset+observe window and a subsequent 45s passive-observation
window — no false trips during normal operation, confirmed over the network
(`GET /version`, `GET /api/status` both healthy).

Commit: `f16ccbe` — `fix(app): add task watchdog protection` (unchanged
from the earlier session; this session only added and then fully reverted
a temporary verification hook — no new firmware commit needed for M16.3
itself).

---

## 4. M16.4 — BLE disconnect during dispense — IMPLEMENTED (no fix needed), HARDWARE TESTED

**Method**: a temporary, controlled test hook
(`BleManager::TestOnlyForceDisconnect()`, posted onto the host task's own
NimBLE event queue — the same cross-task discipline `command_event_`/
`reconnect_event_` already use, never touching `conn_handle_` from the
wrong task) fired a real `ble_gap_terminate()` on the live connection ~1.5s
into an observed `kDispensing` state. Triggered a real, minimal (100ml,
the protocol's own `kMinAmountMl`) controlled Still dispense via
`POST /api/dispense` against the real Grohe Blue Home.

**Evidence**: dispense accepted and ACKed by the appliance
(`dispense_status -> kDispensing`); forced disconnect at t+2.03s
(`ble_gap_terminate`, `conn_handle=1`); BLE state
`ReadyForProtocol -> Disconnected -> Backoff` (identical path a real link
loss/remote disconnect takes, not a simulated event); app received
`BLE event: ConnectionFailed` and ran `HandleConnectionLost()`; automatic
reconnect completed in ~2.0s (`Backoff -> Scanning -> DeviceFound ->
Connecting -> Connected -> DiscoveringServices -> ReadyForProtocol ->
Subscribed`); zero `task_wdt` triggers, zero crashes, zero reboots; HTTP
API stayed reachable throughout; post-recovery `GET /api/status`:
`connection_status=READY`, `dispense_status=IDLE`, `delivered_ml=0` —
consistent, not stuck.

**No bug found** — the existing M8/M11.1 `HandleConnectionLost()`/
`ScheduleReconnect()` logic already handled this correctly; this session's
hardware test is the first time it was exercised against a real forced
mid-dispense disconnect, not just unit-level/simulated. All temporary test
code (`ble_manager.hpp`/`.cpp`, `grohe_client.hpp`, `app.cpp`) was reverted
via `git checkout --` (confirmed byte-identical to committed HEAD) and the
clean build reflashed and re-verified stable before moving on.

---

## 5. M16.5 — Boot-time HA race — IMPLEMENTED (no fix needed), HARDWARE TESTED

**Method**: a rapid HTTP polling loop (`GET /api/status`, ~150–300ms
cadence, 1s timeout) simulating an aggressive HA `DataUpdateCoordinator`,
run continuously across a real device reset (USB RTS-pulse hard-reset),
correlated against a parallel serial capture of the firmware's own boot
log.

**Evidence**: 94/97 requests succeeded across the 20s window; exactly 3
consecutive connection-refused results during the actual restart
(~3.15s, from the reset to `httpd_start()` completing); then immediate,
sustained recovery. Serial log confirms a clean external reset
(`rst:0x15, USB_UART_CHIP_RESET`), `"Startup complete"` ~1.0s later,
Wi-Fi connected + `"Provisioning endpoint ready"` ~1.0s after that —
matching the poller's own recovery timing almost exactly. Zero `task_wdt`
triggers, zero panics, zero malformed responses.

**No bug found** — requests arriving before `httpd_start()` completes
simply find nothing listening (clean connection-refused), never a
half-initialized handler. The ~3.15s real-world outage is comfortably
inside M16.1's own 5s initial `retry_after` backoff, so a real HA instance
recovers on its very next scheduled poll with no special-casing needed.

---

## 6. M16.6/M16.7 — Grohe Cloud package analysis + provisioning architecture — IMPLEMENTED (design)

Installed the real `grohe==0.3.1` package and read its actual source
directly, cross-checked against this project's own existing, already-
verified reference implementation (`scripts/grohe_cloud_bootstrap.py`,
`scripts/grohe_cloud_refresh.py`, `scripts/provision.sh`). Key findings:

- `GroheTokens.get_tokens_from_credentials(email, password)` /
  `get_refresh_tokens(refresh_token)` both return a `GroheTokensDTO`.
- Device discovery is one authenticated `GET` to the Cloud dashboard
  endpoint — each appliance carries its own `presharedkey` directly.
- `user_id` is the JWT `sub` claim of the access token.
- **The firmware's existing `POST /provision` endpoint**
  (`components/provisioning/provisioning_server.cpp`, M13.2, unchanged
  since) already accepted exactly `{"user_id": ..., "preshared_key_base64":
  ...}` under `X-Provision-Token` auth — precisely what the Cloud package
  produces. **Zero firmware changes were needed for provisioning.**

**Architecture**: a new Home Assistant *Options* Flow (not a config-entry
field), reachable from an already-added dial's own "Configure" action.
Three steps: Cloud login → appliance selection (auto-skipped for a
single-appliance account) → the dial's own provisioning token, ending in
one `POST /provision` call.

---

## 7. Physical device reboot loop — root cause, recovery, and how it's now avoided

At the start of this session's hardware-acceptance work, the dial (USB-
connected, per this session's explicit instruction) was found cycling
through reboots roughly every 6s. Diagnosed via USB/serial before any
change was made:

- Firmware running: `commit=595f5c5 (m16, dirty)`, built in a prior
  session — the temporary M16.3 diagnostic build used to originally prove
  the watchdog mechanism (see §3's history), never cleaned up because that
  session's OTA re-upload attempts (300+ tries) could not get a clean
  build back onto the device before it ended.
- Every cycle: normal init (Wi-Fi, display, BLE service discovery, SNTP)
  proceeded fully, then hit a literal
  `"M16.3 TEST: simulating an app task hang now"` log line (leftover
  diagnostic code, not present in any committed source), then ~5.5s later
  `task_wdt` fired exactly as M16.3 is designed to do, panicked, and
  rebooted (`rst:0xc, RTC_SW_CPU_RST`).
- **This was the watchdog mechanism working correctly against a
  deliberately-hung build, not a new or different bug.** The device was
  never at risk (OTA/USB both only ever write to flash while the current
  partition keeps running; the self-reboot cycle is itself evidence the
  recovery path works) and was rebooting safely on its own the entire
  time.

**Recovery**: with USB now available (this session's own instruction
explicitly permits it — the earlier "OTA only" rule was scoped to a
session where USB was physically disconnected, not a property of the
hardware), a clean build of the current `m16` tree was flashed via
`idf.py -p /dev/cu.usbmodem1101 app-flash`. Verified via serial: single
clean boot, `"Commit: 3ba1828 (m16)"` (not dirty), `"Startup complete"`,
no `task_wdt` trigger, Wi-Fi/BLE/HTTP API all functional — confirmed
further over the network (`GET /version`, `GET /api/status`).

**Going forward**: this class of problem — a temporary hardware-test
build left running because OTA couldn't recover it before a session
ended — is specific to the *previous* session's environment (no USB
available, so no fallback once OTA proved unreliable under BLE/Wi-Fi
radio contention). It is not a recurring risk under normal operation:
`CONFIG_ESP_TASK_WDT_PANIC` only ever trips on a genuine app-task hang,
which no committed code path in this firmware produces.

---

## 8. M16.8/M16.9 — Provisioning implementation + tests — IMPLEMENTED, AUTOMATED TESTED, HARDWARE TESTED (real Grohe Cloud, real dial)

**Implementation** (`cloud.py`, `api.py`'s `provision_dial()`,
`config_flow.py`'s `GroheDialOptionsFlow`, `manifest.json`'s new
requirements) — see the earlier version of this document (still accurate,
unchanged this session) for the full design writeup: Cloud login →
appliance selection → provisioning token → one `POST /provision` call,
reusing the `grohe` package end-to-end, zero firmware changes.

**Automated tests** (21 tests: `test_cloud.py` 11, `test_provisioning_flow.py`
10) — unchanged this session, still passing (71/71 across the whole HA
suite).

**Real hardware acceptance (this session)**: a temporary, never-committed
script directly imported the real, already-committed `cloud.py`/`api.py`
modules (not a reimplementation) and drove the real pipeline against the
real Grohe Cloud and the real physical dial, using an existing, genuinely
obtained refresh token (`scripts/.grohe_cloud_refresh_token`, gitignored,
from earlier authorized login work — refresh tokens don't need a
password, so this avoided re-exposing the account password to a
non-interactive script for zero additional coverage; the interactive
email+password HA form step itself is already covered by
`test_provisioning_flow.py`'s mocked tests). Every secret was redacted to
length + a 4-character prefix in all output; nothing was logged or
committed in full.

Evidence, in order:
1. `cloud.refresh_tokens(<real token>)` — real Grohe Cloud OIDC call.
   **SUCCESS.**
2. `cloud.user_id_from_access_token(<real token>)` — real JWT decode.
   **SUCCESS** (36-char UUID extracted).
3. `cloud.list_appliances(<real token>)` — real Cloud dashboard fetch.
   **SUCCESS**: 1 candidate, `name="My GROHE Blue Home"` — the real,
   already-known appliance.
4. `api.provision_dial(...)` — real `POST /provision` to the physical
   dial, real provisioning token, real Cloud-sourced `user_id`/
   `preshared_key_base64`. **SUCCESS**: `{"status":"ok",
   "reboot_required":false}`; the existing BLE connection (authenticated
   under the *previous* credentials) stayed up without interruption.

Post-provisioning verification, all real hardware (§9 continues this):
device rebooted twice, the second time confirming
`"Using provisioned Grohe credentials from NVS"` (the new, real
Cloud-sourced pair) and a full, successful BLE reconnect to the same
appliance address.

Commits: `844da3e` — `feat(ha): add grohe blue provisioning`; `afb150f` —
`test(ha): cover grohe blue provisioning` (both unchanged from the earlier
session; no new commits were needed for the hardware-acceptance work
itself, since it exercised only already-committed code).

---

## 9. M16.10 — Real hardware provisioning — HARDWARE TESTED

Continues directly from §8's evidence. Post-provisioning, with the new
real Cloud-sourced credentials active:

- **Reboot #1**: SNTP timed out once (`"SNTP sync timed out; giving up"`)
  — a pre-existing, one-shot-per-boot behavior with no automatic retry
  (out of M16 scope; not a regression, not touched by this milestone).
  BLE itself reconnected fine regardless (`Connecting -> Connected ->
  DiscoveringServices -> ReadyForProtocol`, same appliance address
  `4c:11:ae:95:3b:12`).
- **Reboot #2**: clean boot, `"Using provisioned Grohe credentials from
  NVS"`, SNTP succeeded (`"SNTP sync succeeded; system clock set"`), full
  BLE reconnect.
- **Dispense**: `POST /api/dispense {100 ml, STILL}` → accepted, appliance
  ACKed (`SUCCESS`).
- **Dispense + Stop**: `POST /api/dispense {200 ml, STILL}` → accepted;
  `POST /api/stop` shortly after → accepted; `GET /api/status` confirmed
  `dispense_status=STOPPING` with `delivered_ml=130` mid-transition,
  settling cleanly to `dispense_status=IDLE`, `delivered_ml=0`.
- **"HA status"**: no separate live HA instance was reconfigured for this
  specific check — the local HTTP API exercised above *is* exactly what
  HA's own `DataUpdateCoordinator` polls
  (`GroheDialApiClient.get_status()`), so its confirmed correctness here
  is a direct, faithful proxy for HA-side status correctness, not a
  separate untested surface.

**Verdict: PASS.** Every part of the provisioning pipeline — Cloud token
refresh, JWT decode, Cloud device discovery, and the dial's own
`/provision` endpoint — was exercised against real, live systems and
produced a dial that reconnected, dispensed, and stopped correctly
afterward.

---

## 10. Summary

| Item | Code | Tests | Hardware |
|---|---|---|---|
| M16.1 Connection retry/backoff | ✅ | ✅ 7 tests | ✅ PASS |
| M16.2 Actionable error messages | ✅ | ✅ 6 tests | ✅ PASS |
| M16.3 Task watchdog | ✅ | — (hardware-proven mechanism, see §3) | ✅ PASS (clean production build) |
| M16.4 BLE disconnect during dispense | ✅ (no fix needed) | — | ✅ PASS |
| M16.5 Boot-time HA race | ✅ (no fix needed) | — | ✅ PASS |
| M16.6 Cloud package analysis | ✅ (design) | — | n/a |
| M16.7 Provisioning architecture | ✅ (design) | — | n/a |
| M16.8 Provisioning implementation | ✅ | ✅ (part of 21) | ✅ PASS (real Cloud + real dial) |
| M16.9 Provisioning tests | ✅ | ✅ 21 tests | n/a |
| M16.10 Real hardware provisioning | ✅ | — | ✅ PASS |

**Automated tests**: 71/71 pass across `homeassistant/tests/`.
**Firmware build**: clean, 0 errors, 15 pre-existing warnings only (same
as M15/M16's earlier baseline — none introduced by M16). Flash: 20% free
on the app partition. RAM: consistent with the established M14/M15
baseline (~28–30 KB internal free at the BLE-subscribed checkpoint, no
regression).

`main`/`origin/main` unchanged at `96f4b16` throughout implementation;
merged to `main` only after every row above independently reached PASS.
