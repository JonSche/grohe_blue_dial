# M15 — Native Home Assistant Integration (Local HTTP, MQTT Removed)

> **Status: implemented and hardware-verified for the core vertical slice.** M14 (`c31058a`) is untouched. **No commit has been made** — this entire milestone is still uncommitted, pending review. Tags used throughout: **IMPLEMENTED**, **TESTED** (automated), **HARDWARE TESTED** (real device/real Grohe Blue), **NOT TESTED**, **UNKNOWN**.

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

**Build**: clean, zero new warnings (13 pre-existing warnings only, unchanged from M14).
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
- `Status()`/`Config()` are plain reads (documented as safe on this single-core chip, mirroring `BleManager::State()`'s own established precedent); `SetConfig()` needs no synchronization at all — `dial_settings_` is httpd-task-exclusive after boot (the app task's own one-time `ApplySettings()` read already happened during startup).

**Hardware-verified real dispense/stop round-trip** (see §7) — the queue hand-off, `DialController`'s new entry points, and the existing BLE/HMAC path all worked correctly together on the first attempt.

### 3.3 Authentication (M15.3) — IMPLEMENTED, HARDWARE TESTED

New, separate token (`X-Api-Token`, `provisioning::ApiSecretProvider`/`LocalApiSecretProvider`, gitignored `api_secret_local.hpp` — same pattern as OTA's/Provisioning's own tokens) — deliberately **not** a reuse of the provisioning token: this one gates an endpoint that can make the appliance dispense water immediately, a materially more consequential scope than "can overwrite stored credentials" (inert until the next command). Every `/api/*` endpoint requires it, including `GET /api/status` — no public/protected split, for simplicity. Constant-time comparison (`ConstantTimeEquals()`, duplicated a third time — see `docs/m15_ha_integration_analysis.md` §9 for why this was flagged, and the decision to keep duplicating rather than extract a shared helper for this milestone).

**Replay protection**: deliberately not added at the HTTP layer. The BLE protocol already carries its own timestamp, rejected by the real appliance if stale (`TIMESTAMP_EXPIRED`) — the point that actually matters already has protection; an HTTP-level nonce scheme would be exactly the unneeded complexity this milestone was asked to avoid.

**Secrets never logged** — verified by reading every new log line in `provisioning_server.cpp`: only lengths/outcomes, consistent with the pre-existing discipline.

---

## 4. Native Home Assistant integration (M15.5–M15.8) — IMPLEMENTED, mix of TESTED and NOT TESTED (see below)

New Python package: **`homeassistant/custom_components/grohe_dial/`** in this same repository (a deliberate placement choice, not a silent one — no separate HA config directory/repo was available in this environment; documented here explicitly).

### 4.1 Config Flow (IMPLEMENTED, TESTED)

Local-only for this vertical slice, per your explicit instruction (no Cloud login required to get a working device):

```
Add Integration → "Grohe Dial"
  host, port (default 8080), API token
    → GET /api/status (proves reachability + auth)
    → unique_id = host (see §4.4 — a documented limitation, not an oversight)
    → Device created
```

Reauth flow (`async_step_reauth`) implemented for an expired/rotated token.

### 4.2 Device/entity model (IMPLEMENTED, TESTED)

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

### 4.3 Services (IMPLEMENTED, NOT TESTED beyond schema/registration)

`grohe_dial.dispense` (`amount_ml`, `water_type`, entity-targeted) and `grohe_dial.stop` — standard HA entity-service pattern (`cv.make_entity_service_schema`), resolving the target entity back to its config entry's coordinator. Registered once, globally, guarded against double-registration across multiple config entries. Schema/registration verified by the full-setup test (§5); the actual dispense/stop *call path* through these specific service handlers was not separately exercised (it shares its implementation with `button.py`'s already-tested path, but the service-specific target-resolution code (`_coordinator_for_entity`) has no dedicated test — **NOT TESTED**).

### 4.4 Cloud login / Grohe Blue linking — NOT IMPLEMENTED, deferred

Per your explicit instruction ("Für den ersten funktionierenden Vertical Slice darf die Konfiguration zunächst lokal erfolgen"), this milestone does not implement Cloud login or `ha-grohe_smarthome` linking. The prior analysis phase (`docs/m15_ha_integration_analysis.md` §5.4) verified, against `ha-grohe_smarthome`'s actual source, that **Option A (reusing its `entry.runtime_data`) is technically feasible** — not re-verified or implemented here. Two concrete, known limitations as a result:

1. **`via_device` linking to a Grohe Blue Home device is not implemented.** Would need either the cloud-login integration above, or a manually-entered `appliance_id`.
2. **Config-entry `unique_id` is the dial's host/IP, not its true stable Wi-Fi-MAC identity.** The firmware's `GET /version` does not currently expose the MAC (only version/commit/branch); adding it was considered but deferred to avoid further firmware changes within this already-large milestone. A dial that changes IP (no DHCP reservation) would need to be re-added in HA, not silently re-matched. **Documented risk, not hidden.**

---

## 5. Automated tests — TESTED (genuinely run, not just written)

Ran against a real, current Home Assistant core (2026.9.1) and `pytest-homeassistant-custom-component` 0.13.364, installed fresh into a throwaway venv (`homeassistant/.venv-test/`, gitignored) specifically to verify this code, not left as unverified claims:

```
14 passed in 0.38s
```

| File | What | Result |
|---|---|---|
| `tests/test_api.py` (9 tests) | `GroheDialApiClient` against a real `aiohttp` `TestServer` fake of the firmware's own endpoints — status/config GET/POST, dispense accept/reject, stop reject, invalid token | **TESTED, all pass** |
| `tests/test_config_flow.py` (4 tests) | Success, invalid auth, cannot-connect, duplicate-host-abort — against HA's real `config_entries.flow` machinery | **TESTED, all pass** |
| `tests/test_init.py` (1 test) | Full `async_setup_entry` → device registry entry created with correct identifiers, **all 9 entities** across all 5 platforms registered under it, both services registered, clean unload | **TESTED, all pass** |

This test run **found and fixed a real bug**: `DeliveredAmountSensor`'s original `device_class="volume"` + `state_class=MEASUREMENT` combination is rejected by Home Assistant's own entity validation (volume sensors require `total`/`total_increasing`). Fixed by removing both attributes (see §4.2's own reasoning) — verified by re-running the same test afterward.

**Every module** (`__init__.py`, `api.py`, `config_flow.py`, `coordinator.py`, `entity.py`, every platform file, `services.py`) was also confirmed to **import cleanly** against real Home Assistant core in isolation, before the fuller test suite existed.

**Not tested / not run in this environment**: the actual Home Assistant UI (Settings → Devices & Services → Add Integration) was never opened in a browser — only the underlying `config_entries` API the UI itself calls. No live HA instance with this integration installed via HACS or manually copied into a real `config/custom_components/` was exercised end-to-end.

---

## 6. Regression testing (M15.11) — HARDWARE TESTED

Firmware, after the full MQTT removal + HTTP API implementation, tested together on real hardware (3+ separate boot cycles across this milestone, plus one final combined pass):

| Area | Result |
|---|---|
| BLE (discovery, connect, GATT discovery, characteristic caching, subscribe) | **HARDWARE TESTED**, working, 0 crashes across every run |
| OTA (`GET /version`) | **HARDWARE TESTED**, live-curled, correct response |
| Provisioning (`POST /provision`, unauthorized → 401) | **HARDWARE TESTED**, live-curled |
| Local API — all 5 endpoints, auth, malformed JSON, invalid amount/water_type, missing field, stop-while-idle (409) | **HARDWARE TESTED**, every case behaved exactly as designed |
| **Real dispense + live status + stop**, against the actual Grohe Blue Home | **HARDWARE TESTED** — 100 mL STILL (full cycle), then 300 mL MEDIUM with a mid-flight `dispense_status: DISPENSING`/`delivered_ml` observation and a successful `POST /api/stop` mid-pour, confirmed via both the HTTP response and the firmware's own serial log (`Dispense requested via API`/`Stop requested via API`, `result=0`) |
| Crashes | **0** across every test run this milestone |
| New compiler warnings | **0** (13 pre-existing warnings only, unchanged from M14) |

---

## 7. RAM / Flash — real measurements, not estimates

### 7.1 Flash

| Build | Size | Δ vs. M14 |
|---|---:|---:|
| M14 baseline (with MQTT) | 1,767,776 B | — |
| M15.1 (MQTT removed) | 1,587,536 B | **−176.0 KiB** |
| M15 final (+ local HTTP API) | 1,619,424 B | **−144.9 KiB** |

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

---

## 8. Known limitations / deferred (explicit, not silently dropped)

- Cloud login / `ha-grohe_smarthome` linking (§4.4) — deferred, not started.
- `via_device` Grohe Blue ↔ Dial linking — blocked on the above.
- Dial's stable MAC-based `unique_id` — deferred; host/IP used instead.
- CO₂/filter/consumables — architecturally out of reach of this API (BLE never carries it); would require the cloud-login work above regardless.
- Multi-appliance BLE disambiguation (derived device name from serial number, `docs/m15_ha_integration_analysis.md` §1.3) — not implemented; today's single-appliance service-UUID-only match is unchanged.
- `grohe_dial.dispense`/`grohe_dial.stop` services' own target-resolution code path — not independently tested (shares implementation with the already-tested button path).
- No live HA UI / HACS installation exercised — only the underlying `config_entries` machinery, via automated tests.

## 9. Git

**No commit made.** All M15 changes remain in the working tree, deliberately staged for your review before any commit — see the accompanying summary for the exact `git status`/`git diff --stat`.
