# M15 — Native Home Assistant Integration (Local HTTP, MQTT Removed)

> **Status: COMPLETE.** Implemented, automated-tested, and hardware-accepted end to end — including the real Home Assistant integration (installed on a real HA instance, real Config Flow, real device/entities, real `grohe_dial.dispense`/`grohe_dial.stop` service calls that produced real physical water dispenses and a real mid-pour stop on the actual Grohe Blue Home). M14 (`c31058a`) is untouched. Committed as four commits on `main`: `bb10480` (feature), `d8ff91c`, `141db73`, `57d2b7e` (three hardening/bugfix follow-ups, see §6a). Tags used throughout: **IMPLEMENTED**, **AUTOMATED TESTED**, **HARDWARE TESTED** (real device/real Grohe Blue/real Home Assistant), **NOT TESTED**, **DEFERRED**.

---

## 1. Architecture (IMPLEMENTED, HARDWARE TESTED)

```
Home Assistant
      │  native integration, local HTTP (port 8080)
      ▼
  Grohe Dial (this firmware)
      │  BLE
      ▼
  Grohe Blue Home
```

MQTT is **fully removed** from the firmware. The dial's local HTTP API (extending the existing `components/provisioning/` httpd instance — no third `esp_http_server` task) is now the only network control surface, alongside the pre-existing OTA and provisioning endpoints. A new, separate Python package (`homeassistant/custom_components/grohe_dial/`) is a real Home Assistant Config-Entry-based integration, not MQTT Discovery.

---

## 2. MQTT removal (M15.1) — IMPLEMENTED, HARDWARE TESTED

Removed in full:
- `components/dial_mqtt/` (entire component: client, HA Discovery, config — 1,313 lines) deleted.
- `components/app/CMakeLists.txt`'s `dial_mqtt` dependency, `app.hpp`'s includes/member, `app.cpp`'s `Init()`/`OnStateChanged()` calls.
- `sdkconfig.defaults`'s `CONFIG_MQTT_TRANSPORT_SSL=n` block (with it, ESP-IDF's own `mqtt`/esp-tls component drops out of the build entirely, not just unused).
- `.gitignore`'s `mqtt_config_local.hpp` entry.
- `mem_diag.hpp`'s `"mqtt_task"` candidate name and stale references.
- Docs: `docs/mqtt_ha_discovery_plan.md` deleted; `docs/ARCHITECTURE.md`, `docs/ROADMAP.md`, `SECURITY.md` updated to point at this document instead of describing removed code as current.

Verified by repository-wide grep for `mqtt`/`MqttClient`/`dial_mqtt`/topic names after removal — every remaining hit is either historical commentary (explicitly dated, e.g. "M13.3 investigation") or this document's own analysis; none is live code.

**Build**: clean, zero new warnings (15 pre-existing warnings only — `-Wmissing-field-initializers`/`-Wextra` in files M15 never touched — unchanged from M14, re-verified on the final `idf.py fullclean && idf.py build`, see §6b).
**Hardware**: 3 consecutive boot cycles, 0 crashes, full BLE chain + OTA + Provisioning all functional (see §7).

---

## 3. Local HTTP API (M15.2–M15.4) — IMPLEMENTED, HARDWARE TESTED

Lives on the existing Provisioning httpd instance (port 8080) — `config.max_uri_handlers` raised from 1 to 6; no new httpd task/stack. Five new endpoints, `components/provisioning/provisioning_server.cpp`:

| Endpoint | Auth | Purpose |
|---|---|---|
| `GET /api/status` | `X-Api-Token` | Direct JSON mirror of `dial_state::DialState` — connection/time/dispense status, water type, amount, delivered_ml, appliance_response. **No second state store** — reads the exact same struct `ui::UiManager::Render()` does. |
| `GET /api/config` | `X-Api-Token` | `default_amount_ml`, `amount_step_ml`, `default_water_type` — direct mirror of `settings::DialSettings`. |
| `POST /api/config` | `X-Api-Token` | Partial update (only fields present in the body change), through the existing `settings::DialSettingsStore::Set()` — no new persistence logic. |
| `POST /api/dispense` | `X-Api-Token` | `{"amount_ml": int, "water_type": "STILL"\|"MEDIUM"\|"SPARKLING"}` |
| `POST /api/stop` | `X-Api-Token` | No body. |

**CO₂/filter status**: not part of this API. Confirmed (this milestone's own analysis phase, `docs/m15_ha_integration_analysis.md` §1.3, cross-validated against the GroheWatersystems decompilation) that this data is **cloud-only, never available over BLE** — the dial has no way to know it, so the API cannot expose it either. Not a gap in this implementation; a hard constraint of the BLE-only architecture.

### 3.1 Reused the existing dispense/stop call chain exactly — no second implementation

Traced in full before writing any new code (`docs/m15_ha_integration.md`'s own earlier revision, verified against `dial_controller.cpp`/`grohe_client.cpp`/`grohe_protocol.cpp` directly):

```
EncoderEvent → DialController::HandleEvent() → DialAction
    → GroheClient::RequestDispense()/RequestStop() → HMAC → BLE
```

Two new, additive `DialController` methods (`RequestDispenseAction()`, `RequestStopAction()`) mirror `HandleEvent()`'s existing `kShortPress` branches line-for-line — same `command_pending_` debounce, same `dispense_status` gate, same `pending_dispense_amount_ml_` snapshot. They produce the exact same `DialAction` enum `HandleEvent()` already does; everything downstream (`GroheClient::RequestDispense()`, HMAC signing, the BLE write, `HandleCommandOutcome()`) is **100% unmodified, shared code**.

### 3.2 Cross-task hand-off (IMPLEMENTED, HARDWARE TESTED)

`DialController`/`GroheClient` may only be touched from `App::Run()`'s own app task (established, repo-wide invariant — the same one `BleManager` enforces for its own state). The httpd task the new endpoints run on never touches them directly. New mechanism, mirroring `BleManager::command_queue_` exactly:

- New tiny component `components/dial_api/` — an abstract interface (`dial_api::DialApiHandler`) both `app` and `provisioning` depend on, avoiding a circular CMake dependency (`app` already `REQUIRES provisioning`).
- `app::App` implements the interface. `RequestDispense()`/`RequestStop()` (called from the httpd task) push a command onto a depth-1 `QueueHandle_t`, then block (1 s bounded timeout) on a depth-1 result queue.
- `App::Run()`'s own loop drains the command queue once per 20 ms tick, right where the encoder-poll callback already lives, and posts the result back via `xQueueOverwrite()`.
- `Config()` is a plain read (no synchronization needed — `dial_settings_` is httpd-task-exclusive after boot, the app task's own one-time `ApplySettings()` read already happened during startup); `SetConfig()` likewise.
- `Status()` **also** goes through the same command/result queue mechanism as of a follow-up fix (`d8ff91c`, see §6a) — a direct cross-task read was found, on independent review, to risk observing a torn combination of fields mid-transition (not just a stale-but-coherent snapshot). Closed by routing every `/api/status` read through the app task too, the same place every write already happens.

**Hardware-verified real dispense/stop round-trip** (see §6b) — the queue hand-off, `DialController`'s new entry points, and the existing BLE/HMAC path all worked correctly together, including through the real Home Assistant integration end to end.

### 3.3 Authentication (M15.3) — IMPLEMENTED, HARDWARE TESTED

New, separate token (`X-Api-Token`, `provisioning::ApiSecretProvider`/`LocalApiSecretProvider`, gitignored `api_secret_local.hpp` — same pattern as OTA's/Provisioning's own tokens) — deliberately **not** a reuse of the provisioning token: this one gates an endpoint that can make the appliance dispense water immediately, a materially more consequential scope than "can overwrite stored credentials" (inert until the next command). Every `/api/*` endpoint requires it, including `GET /api/status` — no public/protected split, for simplicity. Constant-time comparison (`ConstantTimeEquals()`, duplicated a third time — see `docs/m15_ha_integration_analysis.md` §9 for why this was flagged, and the decision to keep duplicating rather than extract a shared helper for this milestone).

**Replay protection**: deliberately not added at the HTTP layer. The BLE protocol already carries its own timestamp, rejected by the real appliance if stale (`TIMESTAMP_EXPIRED`) — the point that actually matters already has protection; an HTTP-level nonce scheme would be exactly the unneeded complexity this milestone was asked to avoid.

**Secrets never logged** — verified by reading every new log line in `provisioning_server.cpp`: only lengths/outcomes, consistent with the pre-existing discipline.

---

## 4. Native Home Assistant integration (M15.5–M15.8) — IMPLEMENTED, HARDWARE TESTED end to end

New Python package: **`homeassistant/custom_components/grohe_dial/`** in this same repository (a deliberate placement choice, not a silent one — no separate HA config directory/repo was available in this environment; documented here explicitly).

### 4.1 Config Flow (IMPLEMENTED, HARDWARE TESTED — see §6a for the float-port bug found and fixed here)

Local-only for this vertical slice, per your explicit instruction (no Cloud login required to get a working device):

```
Add Integration → "Grohe Dial"
  host, port (default 8080), API token
    → GET /api/status (proves reachability + auth)
    → unique_id = host (see §4.4 — a documented limitation, not an oversight)
    → Device created
```

Reauth flow (`async_step_reauth`) implemented for an expired/rotated token.

### 4.2 Device/entity model (IMPLEMENTED, HARDWARE TESTED — all 9 entities confirmed on a real device)

| Entity | Platform | Why | Writes through |
|---|---|---|---|
| BLE connection | `binary_sensor` (connectivity) | Automation-grade, on/off | — |
| Connection status (detailed) | `sensor` (enum, diagnostic) | Diagnostic, not automation material | — |
| Dispense status | `sensor` (enum: idle/dispensing/stopping/finished/failed) | State, not an action | — |
| Delivered amount | `sensor` (mL) | Live progress; deliberately **no** `device_class`/`state_class` — resets every dispense, not a cumulative meter (HA's own validation rejected `device_class=volume` + `state_class=measurement` — caught by this milestone's own test suite, see §5) | — |
| Default water type | `select` | Fixed 3-value enum, user-settable | `POST /api/config` |
| Default amount / step | `number` × 2 | Bounded numeric | `POST /api/config` |
| Dispense | `button` | One-shot action, **not** a `switch` (no persisted on/off state) — fires with the dial's *currently dialed-in* amount/water_type, matching the physical encoder's own semantics | `POST /api/dispense` |
| Stop | `button` | Same reasoning | `POST /api/stop` |

`Grohe Blue Home └── Grohe Dial` via `via_device`: **NOT IMPLEMENTED this milestone** — see §4.4.

### 4.3 Services (IMPLEMENTED, HARDWARE TESTED — real bug found and fixed here, see §6a)

`grohe_dial.dispense` (`amount_ml`, `water_type`, entity-targeted) and `grohe_dial.stop` — standard HA entity-service pattern (`cv.make_entity_service_schema`), resolving the target entity back to its config entry's coordinator. Registered once, globally, guarded against double-registration across multiple config entries.

Both services were exercised for real against the physical dial through a real Home Assistant instance — `grohe_dial.dispense` produced real Still/Medium/Sparkling water dispenses; `grohe_dial.stop` stopped a real, in-progress pour. This is what actually caught the service-registration bug fixed in `57d2b7e` (see §6a): the schema/registration-only automated test that existed before hardware acceptance could not have caught it, because the bug was in *how the handler was awaited*, not in schema validation — the call "succeeded" with zero effect. Two new regression tests (`test_dispense_service_success_path_actually_reaches_the_api`, `test_stop_service_success_path_actually_reaches_the_api` in `tests/test_integration_flow.py`) now assert the service call actually reaches the (faked) dial, not just that it validates.

### 4.4 Cloud login / Grohe Blue linking — NOT IMPLEMENTED, deliberately deferred

> **Update — since resolved.** The `via_device` link and the stable
> MAC-based `unique_id` this section describes as deferred were both
> implemented, hardware-tested (including against the project's own live
> Home Assistant instance), and closed on `feature/m15-completion`. See
> [`docs/m15_completion.md`](m15_completion.md) for the full account. The
> analysis below is left as-written — it's what M15 actually shipped with at
> the time, and the reasoning for staying local-only (no `ha-grohe_smarthome`
> fork) still held for how the eventual link was implemented: a soft,
> optional Device Registry link using the Cloud `appliance_id` this
> integration's own provisioning flow already fetches, not a fork or
> extension of that integration.

This milestone does not implement Cloud login or `ha-grohe_smarthome` linking — the local-only Config Flow (host/port/API token, §4.1) is the deliberate, permanent architecture for *this* integration, not a placeholder. A dedicated architecture review (post-M15, before this documentation update) explicitly evaluated forking/extending `ha-grohe_smarthome` to add the dial as a second device type and concluded **against** it for now: that project's device model, discovery, and coordinator are all built around its cloud API client (`iot_class: cloud_polling`, devices enumerated from a cloud dashboard call, `GroheTypes` sourced from an external PyPI package) — the dial has no cloud registration at all, so almost nothing would actually be reusable, and a fork would mean carrying an entire unrelated cloud-integration codebase for near-zero shared code. **Not an M15 blocker** — a possible future direction, not started, not required for M15 or any planned M16 work.

Two concrete, known limitations as a result of staying local-only:

1. **`via_device` linking to a Grohe Blue Home device is not implemented.** Would need either a cloud-login integration, or a manually-entered `appliance_id`.
2. **Config-entry `unique_id` is the dial's host/IP, not its true stable Wi-Fi-MAC identity.** The firmware's `GET /version` does not currently expose the MAC (only version/commit/branch). A dial that changes IP (no DHCP reservation) would need to be re-added in HA, not silently re-matched. **Documented risk, not hidden** — unchanged by this milestone's hardware acceptance, not re-evaluated.

---

## 5. Automated tests — AUTOMATED TESTED (genuinely run, not just written)

Ran against a real, current Home Assistant core (2026.9.1) and `pytest-homeassistant-custom-component` 0.13.364, installed fresh into a throwaway venv (`homeassistant/.venv-test/`, gitignored) specifically to verify this code, not left as unverified claims. Grown across the hardware acceptance pass (§6a/§6b) as real gaps were found:

```
37 passed in ~1s
```

| File | What | Result |
|---|---|---|
| `tests/test_api.py` (12 tests) | `GroheDialApiClient` against a real `aiohttp` `TestServer` fake of the firmware's own endpoints — status/config GET/POST, dispense accept/reject, stop reject, invalid token, malformed response, HTTP 500 timeout-reason, connection timeout | **AUTOMATED TESTED, all pass** |
| `tests/test_config_flow.py` (5 tests) | Success, invalid auth, cannot-connect, duplicate-host-abort, float-port normalization (regression, §6a) — against HA's real `config_entries.flow` machinery | **AUTOMATED TESTED, all pass** |
| `tests/test_init.py` (2 tests) | Full `async_setup_entry` → device registry entry created with correct identifiers, **all 9 entities** across all 5 platforms registered under it, both services registered, clean unload; legacy float-port config entry self-heals (regression, §6a) | **AUTOMATED TESTED, all pass** |
| `tests/test_integration_flow.py` (8 tests) | Full simulated dispense/stop flow through real coordinator + entities against a fake stateful dial (DISPENSING→delivered_ml counting up→FINISHED→IDLE; DISPENSING→STOPPING→IDLE), dial-unavailable/recovery, service schema rejection, **service call success path actually reaching the (fake) dial** (regression for §6a's service-registration bug) | **AUTOMATED TESTED, all pass** |
| `tests/test_status_consistency.py` (10 tests) | Encodes `dial_state.hpp`'s field-consistency contract executably (IDLE/DISPENSING/STOPPING/FINISHED/FAILED/BLE-disconnected) and proves the checker rejects the exact torn combination the concurrency fix (§3.2, `d8ff91c`) addresses | **AUTOMATED TESTED, all pass** |

This suite **found and fixed three real bugs** across its growth: `DeliveredAmountSensor`'s invalid `device_class`/`state_class` combination (original M15 implementation pass), and the two hardware-acceptance bugs in §6a (float port, service registration) — each has a dedicated regression test that reproducibly fails against the old code and passes against the fix.

**Every module** (`__init__.py`, `api.py`, `config_flow.py`, `coordinator.py`, `entity.py`, every platform file, `services.py`) was also confirmed to **import cleanly** against real Home Assistant core in isolation.

**Scope note, honestly stated**: this suite exercises the integration's own logic against fakes (a real `aiohttp` test server standing in for the firmware, or an in-process fake dial state machine) — it does not and cannot exercise the real ESP32 firmware binary or FreeRTOS scheduling. That verification is §6b (real hardware) and, for the concurrency fix specifically, code review (this project has no host-side C++/FreeRTOS test harness — see §5's own scope note in earlier project docs).

---

## 6. Regression testing — HARDWARE TESTED

### 6a. Real bugs found and fixed during hardware acceptance

M15 was committed (`bb10480`) after the firmware-only regression pass documented below, then put through a full **hardware acceptance pass including the real Home Assistant integration** — not just curl against the API, but the integration installed on a real HA instance, added via its real Config Flow, and driven through real `grohe_dial.dispense`/`grohe_dial.stop` service calls against the physical dial. That pass found and fixed two real, independent bugs, each as its own follow-up commit on `m15` (later merged into `main`):

| Commit | Bug | Symptom | Fix |
|---|---|---|---|
| `d8ff91c` | `App::Status()` read `DialController::State()` directly from the httpd task while the app task could be mid-way through a multi-field state transition (e.g. `dispense_status`/`active_dispense_amount_ml`/`delivered_ml` together) — a torn, internally-inconsistent snapshot was possible in principle (found on independent code review, not observed as a live failure) | N/A — closed before it could manifest | `Status()` now round-trips through the same app-task command queue `RequestDispense()`/`RequestStop()` already use, closing the cross-task read entirely; see §3.2 |
| `141db73` | `homeassistant.helpers.selector.NumberSelector` always yields a Python `float` for the port field, regardless of UI input. Config Flow validated it correctly (a local `int()` cast for its own check) but then persisted the **original float** via `async_create_entry(data=user_input)` | Config entry stored `port: 8080.0`; `GroheDialApiClient` built the URL `http://<host>:8080.0` — permanently invalid, integration never connected (`ConfigEntryNotReady`, retried forever) | Config Flow normalizes to `int` before persisting (both the user flow and reauth flow); `__init__.py` additionally re-casts defensively on every setup, so a config entry already created before this fix self-heals without removal/re-add |
| `57d2b7e` | `grohe_dial.dispense`/`grohe_dial.stop` were registered as `lambda call: _async_handle_dispense(hass, call)`. Calling the lambda returns a coroutine, but the lambda itself is not a coroutine function (`asyncio.iscoroutinefunction()` is `False` for it) — Home Assistant's service dispatcher uses exactly that check to decide whether to await the handler, and did not for this one | The service call showed **success** in Home Assistant with **zero** effect — no HTTP request ever reached the dial (confirmed by a live serial-log capture showing 0 bytes during the call), no exception, entity buttons (`button.py`) unaffected (different call path) | Registered genuine `async def` closures instead; two regression tests assert the service call actually reaches a fake dial, and both reproducibly fail against the old lambda registration |

All three fixed, automated-tested (37/37, see §5), deployed to the real Pi, and re-verified on real hardware before being accepted. Neither the pre-acceptance regression pass below nor the original automated test suite could have caught the port/service bugs — both are specifically about the integration's interaction with a *real* HA runtime and a *real* physical device, which is exactly what hardware acceptance exists to catch.

### 6b. Firmware + full-stack regression — HARDWARE TESTED

Firmware, after the full MQTT removal + HTTP API implementation, tested together on real hardware (3+ separate boot cycles across this milestone, plus the hardware acceptance pass):

| Area | Result |
|---|---|
| BLE (discovery, connect, GATT discovery, characteristic caching, subscribe) | **HARDWARE TESTED**, working, 0 crashes across every run |
| OTA (`GET /version`, routing) | **HARDWARE TESTED**, live-curled, correct response. A real `POST /ota` firmware upload was **NOT TESTED** in this milestone — explicitly out of M15's core scope, not a blocker (see §8) |
| Provisioning (`POST /provision`, unauthorized → 401) | **HARDWARE TESTED**, live-curled |
| Local API — all 5 endpoints, auth, malformed JSON, invalid amount/water_type, missing field, stop-while-idle (409), duplicate dispense while one is in flight (409, correctly rejected) | **HARDWARE TESTED**, every case behaved exactly as designed |
| **Real dispense + live status + stop**, against the actual Grohe Blue Home, via curl | **HARDWARE TESTED** — 100 mL STILL (full cycle), then 300 mL MEDIUM with a mid-flight `dispense_status: DISPENSING`/`delivered_ml` observation and a successful `POST /api/stop` mid-pour |
| **Real dispense + stop, via the real Home Assistant integration** (`grohe_dial.dispense`/`grohe_dial.stop`) | **HARDWARE TESTED** — Still, Medium, and Sparkling each dispensed for real via the HA service call; a 1340 mL Still pour was stopped mid-flight via `grohe_dial.stop`, confirmed by the firmware's own serial log (`Dispense requested via API` → BLE write → appliance `SUCCESS`, then ~21s later `Stop requested via API` → BLE write → appliance `SUCCESS`) |
| Home Assistant install/Config Flow/entities, on a real HA instance | **HARDWARE TESTED** — integration installed into a real `custom_components/`, Config Flow completed against the real dial, device created with all 9 entities registered |
| Config round-trip via HA | **HARDWARE TESTED** — `amount_step_ml` changed via the HA `number` entity, verified persisted via a direct `GET /api/config` (not just the HA-side cached display value) |
| Status consistency during a live dispense | **HARDWARE TESTED** — repeatedly polled `/api/status` during real in-progress pours; no torn/inconsistent field combination ever observed, consistent with the `d8ff91c` fix (§3.2) |
| Robustness (40 rapid parallel status/config requests, ~50 min of active use) | **HARDWARE TESTED** — all 200s, no crash/disconnect/watchdog |
| Crashes | **0** across every test run this milestone, including the full hardware acceptance pass |
| New compiler warnings | **0** (15 pre-existing warnings only, unchanged from M14, verified on a clean `idf.py fullclean && idf.py build`) |

---

## 7. RAM / Flash — real measurements, not estimates

### 7.1 Flash

| Build | Size | Δ vs. M14 |
|---|---:|---:|
| M14 baseline (with MQTT) | 1,767,776 B | — |
| M15.1 (MQTT removed) | 1,587,536 B | **−176.0 KiB** |
| M15 (`bb10480`, + local HTTP API) | 1,619,424 B | **−144.9 KiB** |
| M15 final (`57d2b7e`, + status-snapshot queue + fixes) | 1,620,064 B | **−144.3 KiB** |

The +640 B since `bb10480` is the new `api_status_queue_` FreeRTOS queue and the `App::Status()` cross-task hand-off logic added by `d8ff91c` (§3.2/§6a) — the two Home Assistant bugfixes (`141db73`, `57d2b7e`) are Python-only and don't touch the firmware build at all.

### 7.2 RAM (`internal_free`, bytes — `mem_diag` checkpoints, 2–3 hardware runs per stage, consistent/reproducible)

| Checkpoint | M14 baseline | M15 final | Δ |
|---|---:|---:|---:|
| `BOOT` | 166,700 | 167,204–167,332 | ~+500–630 |
| `WIFI_INITIALIZED` | 155,740 | 156,020 | ~+280 |
| `BLE_PRE_INIT` | 91,824 | 92,720–92,768 | ~+900–950 |
| `BLE_INITIALIZED` | 45,572–45,584 | 46,444–46,764 | **~+870–1,190** |
| `PROVISIONING_INIT` | 25,668–27,632 | 38,288–38,756 | **~+11,000–13,000** |
| Final steady-state (M14: `MQTT_DISCOVERY_DONE`; M15: `BLE_SUBSCRIBED`, since nothing runs after — the local API only registers handlers, no connect/publish burst) | 7,884–10,108, largest 4,096–7,168 | **30,548–31,148, largest 12,800** | **~+20,500–23,000 free; largest block ~1.8–3.1×** |

All values 0 crashes, reproducible across repeated boots. **M14's own RAM optimizations are fully intact** — this milestone builds on top of them, doesn't touch `sdkconfig.defaults`'s BLE/NimBLE Kconfig candidates at all.

**Re-measured on `57d2b7e`** (after `d8ff91c`'s status-snapshot queue) during hardware acceptance, multiple boots: `BOOT` internal_free 167,156–167,204; `BLE_SUBSCRIBED` internal_free 30,448–30,476, largest 12,800. Within ~100 B of the pre-fix range above — the extra queue/cache (§7.1) is noise-level against a ~30 KB free steady state, not a measurable regression.

---

## 8. Known limitations / deferred (explicit, not silently dropped)

These are **not** gaps in M15's testing — they are deliberate scope boundaries, unaffected by hardware acceptance:

- **Cloud login / `ha-grohe_smarthome` fork or extension** (§4.4) — deliberately deferred, not started at the time. A dedicated architecture review considered forking/extending `ha-grohe_smarthome` and recommended against it for now (see §4.4) — a possible future direction, **not an M15 blocker**, not planned for M16 either unless separately decided. (M16 did later add a *local* Cloud-login-assisted provisioning flow for BLE credentials — see `docs/m16_reliability_and_provisioning.md` — without forking or depending on `ha-grohe_smarthome`, consistent with this recommendation.)
- **`via_device` Grohe Blue ↔ Dial linking** — was blocked on the above. **Since resolved** — see §4.4's own update note and [`docs/m15_completion.md`](m15_completion.md) §3/§5.
- **Dial's stable MAC-based `unique_id`** — was deferred; host/IP used instead (§4.4, point 2). **Since resolved** — see [`docs/m15_completion.md`](m15_completion.md) §1/§5.
- **CO₂/filter/consumables** — a known technical boundary of the architecture, not a missing feature: the dial's BLE connection to the Grohe Blue Home never carries this data (cloud-only, confirmed against the GroheWatersystems decompilation, §3), so the local HTTP API has nothing to expose here regardless of implementation effort. Unaffected by the cloud-login-assisted *provisioning* work M16 later added — that fetches BLE credentials once, it doesn't add a live Cloud data channel.
- **Multi-appliance BLE disambiguation** (derived device name from serial number) — was not implemented; single-appliance service-UUID-only match. **Since resolved**, via a different mechanism than "derived device name from serial number": a persisted, cryptographically-verified BLE address pin (the appliance's own HMAC verification is the identity proof, not its name). See [`docs/m15_completion.md`](m15_completion.md) §2.
- **Real `POST /ota` firmware-upload path** — routing and `GET /version` are hardware-verified (§6b); an actual upload exercising `esp_ota_write()` was not performed this milestone. Explicitly out of M15's core scope (local HTTP API + HA integration), not a blocker.

## 9. Git

Four commits, all on `main` (fast-forwarded from `m15`, no merge commit, M14 `c31058a` untouched as an ancestor):

```
57d2b7e fix(ha): register services as coroutine functions, not lambdas
141db73 fix(ha): normalize config entry port to int, not float
d8ff91c fix(api): make status snapshots task-safe
bb10480 feat(ha): add local HTTP API and Home Assistant integration
c31058a perf(memory): optimize BLE and HTTP server RAM usage   <- M14, untouched
```

`bb10480` is the original feature commit (this document's §1–§7 core content); the three fixes above it were found and fixed during hardware acceptance (§6a) and merged after independent verification on real hardware.
