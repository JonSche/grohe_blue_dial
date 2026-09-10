# M15 — Native Home Assistant Integration: Analysis & Architecture (Phase A–D)

> Status: **analysis only — no implementation yet.** Nothing in this document has been built. Every claim below is sourced (file:line or URL); anything not directly evidenced is marked **UNKNOWN**. M14 (`c31058a`) is untouched. No commit has been made for this milestone.

---

## 1. What already exists

### 1.1 `grohe_blue_dial` firmware (this repo)

| Piece | State | Evidence |
|---|---|---|
| BLE central role, connects to exactly one Grohe Blue | Working | `components/grohe_ble/ble_manager.cpp` |
| Device targeting | **Service-UUID only** (`33f31bba-…`) — first match wins, no per-device selection, no MAC/name filter | `ble_constants.hpp:25-27`, `ble_manager.hpp:396-404` |
| HMAC-SHA256 command signing | Working, ported from the Python reference | `grohe_auth.cpp`, `grohe_protocol.cpp` |
| Credentials model | `{user_id, pre_shared_key_base64}`, exactly the OIDC `sub` + `GET /v3/iot/dashboard`'s `presharedkey` | `grohe_credentials.hpp:15-25` |
| Credential storage | NVS-backed (`NvsCredentialsProvider`), local-dev fallback header | `grohe_credentials.hpp:44-98` |
| Remote credential provisioning | **Working today**: `POST http://<dial-ip>:8080/provision` with `X-Provision-Token` header, body `{"user_id","preshared_key_base64"}`, `200`/`401`/`400`/`500` | `provisioning_server.cpp` (full read) |
| Remote dispense/stop trigger | **Does not exist.** `GroheClient::RequestDispense()/RequestStop()` are only ever called from `App::Run()`'s encoder-input handling (`app.cpp`) | confirmed by full-repo grep, no other call site |
| MQTT (M13.3) | Working: HA Discovery for **settings only** (`default_amount_ml`, `amount_step_ml`, `default_water_type`) + a read-only `connection_status` sensor. Command topics reach **only** `DialSettingsStore::Set()` | `components/dial_mqtt/`, cross-checked against `SECURITY.md`'s own scope statement |
| Dial's own stable unique ID | Wi-Fi STA MAC, lowercase hex, no separators, via `esp_read_mac(ESP_MAC_WIFI_STA)` (eFuse, no Wi-Fi connection needed) | `dial_mqtt/ha_discovery.hpp:15-24` |
| mDNS/zeroconf | **Not implemented** | repo-wide grep, zero hits |
| Custom DHCP hostname | **Not set** (only the platform default is read/logged) | `wifi_connection.cpp:227-232` |
| OTA endpoint | `GET /version`, `POST /ota` (token-gated), port 80 | `components/ota/` |
| Appliance state visible to the dial | Only `{timestamp, response_code, is_success}` from the write-ack — **no CO₂/filter/consumable data ever reaches the dial**, cross-confirmed independently by the GroheWatersystems decompilation (§1.3 below) | `grohe_protocol.hpp:60-78`, `docs/protocol/03_status_sources.md` |

### 1.2 `grohe_blue_ble` (Python library, sibling repo)

Clean, well-scoped, `bleak`-based async client. Explicit design goal (its own `docs/REVERSE_ENGINEERING.md`): *"Reuse the existing Grohe cloud authentication only to retrieve required credentials… should not depend on Home Assistant."* It has:

- `auth.py` / `credentials.py` — pure HMAC + Base64 logic, `GroheCredentials(user_id, pre_shared_key)` (a plain dataclass; **no cloud-fetching code**).
- `ble.py` — `scan()`, `GroheBleTransport` (bleak wrapper).
- `protocol.py` — payload build/parse, mirrors the ESP32 firmware's own port exactly.
- `client.py` — `GroheBlueClient`, an async context manager (`async with GroheBlueClient(device, credentials) as c: await c.dispense(500, WaterType.SPARKLING)`).
- **No Grohe Cloud OAuth/login code anywhere.** `.env.example` just has `USER_ID=` / `PRESHARED_KEY=` — the user is expected to already have them.
- Good test coverage (`tests/`), CI (`.github/workflows/ci.yml`).

It does **not** currently expose anything that talks to the actual appliance over the network — it's a direct-BLE library, meant to run on whatever machine has the Bluetooth adapter physically near the appliance.

### 1.3 `GroheWatersystems` (decompiled official Android app, evidence repo)

Rigorous, evidence-graded reverse-engineering (`file:line` citations throughout, explicit "Unknowns" sections). Confirms, independently of the two repos above:

- **Cloud API host: `https://idp2-apigw.cloud.grohe.com`** (`resources/res`) — independently reconfirmed twice more below (§1.4).
- **BLE device targeting uses two criteria, not one**: (1) an *exact advertised device name*, **derived from the appliance's serial number** via a specific algorithm (`docs/protocol/01_ble_topology.md` §6, `k.java:39-85`, evidence-graded, not inferred):
  1. prefix `"GROHE_Blue_"` or `"GROHE_BR1_"` by appliance type,
  2. hex-decode `applianceSerialNumber` two characters at a time into ASCII,
  3. if the decoded string is longer than 14 chars: `substring(0,1) + substring(6,14)`, else empty,
  4. `prefix + step 3`.
  (2) the same service UUID the dial already uses. **The dial today only checks the service UUID — this is a real, confirmed gap for multi-device disambiguation.**
- CO₂/filter/consumable/maintenance data is **100% cloud-sourced**, never BLE (`docs/protocol/03_status_sources.md` §0) — this matches and independently confirms this project's own M7 finding.
- A **cloud-side dispense command also exists** (`POST …/appliances/{id}/command`) — the official app can dispense via the cloud, not only BLE. Not part of this project's chosen architecture, but worth knowing it exists.
- `IntegrationRestApi` (§4.4) is a *separate*, appliance-local HTTP surface used only during the appliance's own Wi-Fi onboarding — unrelated to the dial.

### 1.4 External research — existing Home Assistant Grohe integrations (verified via raw source, not just READMEs)

Two custom integrations were found; **the currently maintained one is `Flo-Schilli/ha-grohe_smarthome`** (`koproductions-code`, its earlier Grohe-Blue-only project `ha-groheblue`, is **archived** and superseded by this one).

`ha-grohe_smarthome` (`custom_components/grohe_smarthome/`, `iot_class: cloud_polling`) depends on **PyPI package `grohe==0.3.1`**, whose actual source is **`koproductions-code/grohe`** (verified via PyPI's `project_urls`; a same-named `mickeprag/grohe` repo exists but is unrelated — no `GroheClient`, no `login()`, confirmed by direct source read — a naming collision, not the real dependency).

`koproductions-code/grohe`, read directly (`client.py`, `tokens.py`, `dto/`, `config/config.yaml`):

- **Login** (`tokens.py`, `get_tokens_from_credentials`): scripts the actual Keycloak login form — `GET /v3/iot/oidc/login` (follow redirects to the Keycloak page) → parse the HTML `<form action=…>` → `POST` `{username, password}` to that URL (`application/x-www-form-urlencoded`, no redirect-follow) → Keycloak responds `302` to a custom `ondus://…` deep-link URI → rewritten to `https://…` and `GET`'d → that response body is the token JSON. **Not a documented/stable OAuth grant — a working but fragile screen-scrape of the mobile app's own login flow.** This is consistent with the (initially seemingly contradictory) web-search finding that "programmatic login isn't supported via a clean OAuth flow" — it isn't; this is a workaround.
- **Refresh**: `POST https://idp2-apigw.cloud.grohe.com/v3/iot/oidc/refresh` with `{"refresh_token", "grant_type":"refresh_token"}` → new access+refresh token pair. **Independently confirms `mickeprag/grohe`'s identical endpoint** — cross-validated by two unrelated codebases.
- **User ID**: `jwt.decode(access_token, options={"verify_signature": False})["sub"]` — **identical to this project's own `grohe_blue_ble`/ESP32 approach.**
- **Device model**: `locations → rooms → appliances`, `Appliance.Type.BLUE_HOME = 104` (also `SENSE=101`, `SENSE_GUARD=103`), `appliance_id` is the stable per-appliance ID. This hierarchy **maps directly onto the "Kitchen" grouping in the user's own target diagram.**
- **Relevant API surface**: `GET /dashboard`, `GET/PUT .../appliances/{id}` (info + config), `GET .../appliances/{id}/status`, `POST .../appliances/{id}/command`, `GET .../appliances/{id}/notifications`, `GET .../appliances/{id}/data/aggregated`, pressure-measurement and snooze endpoints (Sense Guard only).
- **No `presharedkey` field in any typed DTO** — `get_dashboard()`/`get_appliance_info()` return raw, untyped dicts, so `presharedkey` (confirmed present in the raw payload by GroheWatersystems' `BlueApplianceStateDto`, §1.3) is reachable, just not modeled as a named attribute anywhere in this library.
- `config/config.yaml` (its declarative entity map) lists a rich `GroheBlueHome` sensor set — remaining CO₂/filter (%, and in liters), replacement/cleaning dates, run-time/cycle counters, online/update-available diagnostics — **directly reusable as a reference entity list** if this project chooses to also surface cloud-sourced consumable data (Sections 5/8.3 below).

`ha-grohe_smarthome`'s own `config_flow.py` (read directly, not summarized): plain `username`/`password` form → `GroheClient(username, password, httpx_client).login()` → on success, `unique_id = "GroheSmarthome"` (a hardcoded singleton — **only one Grohe account per HA instance is supported today**) → `async_create_entry`. Full `async_step_reauth`/`async_step_reauth_confirm`/`async_step_reconfigure` support, plus an options flow.

---

## 2. What's missing for a native HA integration

1. **No remote dispense/stop trigger on the dial at all.** This is the single biggest gap. Neither MQTT command topics nor any HTTP endpoint reach `GroheClient::RequestDispense()`/`RequestStop()` today.
2. **No per-device BLE targeting.** The dial connects to "the first thing advertising the Grohe service UUID," full stop. Fine for one dial + one Blue; breaks down the moment a second Grohe Blue is in range (a neighbor's, a demo unit, etc.).
3. **No network discovery of the dial from HA.** No mDNS, no stable hostname, no BLE advertising (Peripheral role was *disabled* in M14 for RAM — correctly, since it was never used, but it does mean "HA finds the dial by Bluetooth" is not an option either). The only way HA can reach the dial today is a manually-entered IP, or (indirectly) via MQTT if the broker is already configured.
4. **No Grohe Cloud OAuth code anywhere in this project's own repos.** `grohe_blue_ble` explicitly declined to implement it. It exists, verified, in `koproductions-code/grohe` (external, PyPI, MIT-licensed based on its own repo) — see §5.
5. **No formal Dial ↔ Grohe Blue mapping record anywhere** — today "the dial's credentials" implicitly *are* "the dial's one relationship to one appliance." A native integration that wants to show `Grohe Blue Home └─ Kitchen ├─ Blue ├─ Dial` needs an explicit mapping (which `appliance_id`/location/room this dial's credentials belong to) that nothing currently stores.
6. **No local status/command API on the dial for anything beyond the boot-time BLE state machine.** `ApplianceState` (the write-ack) is in-memory only; nothing polls or exposes it over the network today except MQTT's settings/connection_status entities.

---

## 3. Modeling Grohe Blue and Grohe Dial in HA

Grohe's own cloud data model (`locations → rooms → appliances`) already matches the user's target diagram almost exactly. Recommended mapping, using HA's [device registry `via_device`](https://developers.home-assistant.io/docs/device_registry_index/) relationship:

```
Config Entry: "Grohe Cloud Account" (koproductions-code/grohe client, one per Grohe login)
  └─ Device: "Grohe Blue Home — Kitchen"   (identifiers: {("grohe_cloud", appliance_id)})
       └─ via_device ─ Device: "Grohe Dial — Kitchen"  (identifiers: {("grohe_dial", dial_mac)})
```

This is the standard HA pattern for "a physical sub-device that talks to a parent smart-home device" (the same shape used by e.g. Zigbee routers/hubs). It does **not** require the Grohe Blue itself to be a device *this* integration owns exclusively — `ha-grohe_smarthome`, if installed, would keep owning its own `Device` for the same `appliance_id`; HA's device registry does not currently support two integrations sharing one `Device` entry cleanly (each integration's devices are scoped to its own domain's identifiers). **UNKNOWN/open question**, flagged explicitly in §9: whether "true" shared ownership (one Device row, two integrations contributing entities) is achievable, or whether the best available approximation is the `via_device` link above plus a **suggested area** match (`Kitchen`) so both devices at least group visually under the same Area in the UI, which *is* fully supported today.

### 3.1 Entity domain choices (with reasoning, not "model everything as a sensor")

| Data | HA platform | Why |
|---|---|---|
| BLE connection state (Idle/Scanning/Connected/…) | `binary_sensor` (connected/not) + one `sensor` for the detailed state string | A binary "is it working" belongs in automations; the detailed state is diagnostic-only, not automation-worthy on its own |
| Dial online/offline (network reachability) | `binary_sensor`, `device_class: connectivity` | Standard HA convention |
| Connected Grohe Blue identity | `sensor` (diagnostic category), value = appliance name/serial | Informational, not actionable |
| Current water type selection | `select` | Exactly matches HA's own `select` semantics — a small fixed enum the user picks from |
| Amount to dispense | `number` | Continuous(-ish) numeric value with min/max/step — exactly what `number` models; **already implemented** this way for the MQTT-Discovery settings entities, reuse the pattern |
| "Dispense now" | `button` | A stateless, one-shot action — not a `switch` (which implies a persisted on/off state this doesn't have) |
| "Stop/Cancel" | `button` | Same reasoning |
| Ongoing dispense progress | `sensor` (if `PredictDispenseDurationMs()`-based progress is surfaced) or omitted entirely if not reliably knowable — **the appliance itself never reports progress** (GroheWatersystems §7.1: the official app's own "progress bar" is a **client-side time prediction**, not real appliance telemetry) — so a HA "dispense in progress" sensor would be built on the *exact same* prediction model already ported into `PredictDispenseDurationMs()`, not a new capability |
| Error/failure state | `binary_sensor` (`problem` device class) + a `sensor` carrying the last response code, mirroring `dial_state::DispenseStatus::kFailed` (M13.6) | Matches the UI's own existing error concept |
| CO₂/filter/consumables (if pursued at all — cloud-only, **not** available via the dial/BLE) | `sensor` (`%`) + separate `sensor` (liters remaining) + date `sensor`s, `device_class: timestamp` | Direct reuse of `koproductions-code/grohe`'s own `config.yaml` entity list (§1.4) |
| Reset filter / reset CO₂ (cloud-only, if pursued) | `button` | One-shot cloud action, `POST …/command` |
| Default amount / step / water type (already-existing dial settings) | `number`/`number`/`select` — **already built**, MQTT-based today | Out of scope to rebuild; see §6 on the MQTT-vs-native question |

**Deliberately not modeled as entities:** the dial's Wi-Fi credentials, OTA/provisioning tokens, raw HMAC material — none of this belongs in the entity registry; it belongs in the config entry's own encrypted storage (§5).

---

## 4. Config Flow design

Two real architectural forks exist and are flagged explicitly rather than silently resolved:

### Fork 1 — does the coordinator talk to the dial over MQTT or over a new local HTTP API?

The user's own instructions contain both "möglichst eine native Integration, nicht einfach noch mehr MQTT" *and* a request (§7) to evaluate whether `grohe_blue_ble` is "clean enough to build a HA integration from," which implicitly assumes a direct-BLE architecture — that reading conflicts with §2's own "central vision" diagram (`HA → Dial → BLE → Grohe Blue`), which explicitly routes through the dial. I'm treating **the explicitly labeled "zentrale Vision" as authoritative** or this whole project's premise (a dedicated physical dial device) is undermined — but this is a judgment call, not something the evidence alone resolves, and is flagged for your explicit confirmation before Phase E starts.

Given that, "native, not just MQTT" is read as being about the **integration's own architecture** (Config Entries, a real device/entity registry owned by our own `custom_component`, not generic MQTT-Discovery auto-entities) — not necessarily about the wire transport between HA and the dial. Two viable transports for that:

- **Option A — MQTT-backed coordinator.** The custom integration's `DataUpdateCoordinator` subscribes/publishes on the *existing* `dial_mqtt` topics (extended with new dispense/stop command topics — a firmware addition, see §7). Reuses the substantial, already-hardware-verified M13.3 investment. Requires the user to already have HA's own `mqtt` integration configured, which most but not all HA installs have.
- **Option B — direct local HTTP.** A small new authenticated JSON API on the dial (status GET + command POST), following the exact same `esp_http_server` pattern OTA/Provisioning already use, polled by a standard HA `DataUpdateCoordinator`. No MQTT broker dependency; strictly more "native" in the literal sense; but is new firmware surface, not a reuse of M13.3.

**Recommendation: Option B**, specifically because the user's own wording ("nicht einfach noch mehr MQTT") reads as a preference for transport independence, and because this repo already has two working, well-tested precedents (`ota_server.cpp`, `provisioning_server.cpp`) to extend rather than invent from scratch — but this is a recommendation, not a decision made on your behalf.

### Fork 2 — Grohe Cloud login: build our own, or shell out to `koproductions-code/grohe`?

`koproductions-code/grohe`'s login flow (§1.4) is real, working, and MIT-licensed per its own repository — but it screen-scrapes a login form, which is inherently fragile to Grohe changing their login page, and it is a **third-party dependency this project has not audited beyond reading its source once**. Options:

- **Reuse it as a PyPI dependency** (`grohe==0.3.1` or newer) — fastest path, but adopts its fragility and its release cadence as our own.
- **Port the same technique in-repo** (own module, own tests, own maintenance) — matches this project's own established "own the C++ port, cite the Python reference" pattern (`grohe_auth.cpp`'s own header comment) — more control, more upfront work.
- **Require the user to run `ha-grohe_smarthome` (or any Grohe Cloud integration) first, and read its already-stored token/credentials out of HA's own config entry storage**, if HA's integration-to-integration data sharing permits it — **UNKNOWN**, needs a Phase D.5 spike before committing to this path; HA integrations *can* read each other's config entries via `hass.config_entries.async_entries(other_domain)`, but reading another integration's *raw token* out of its entry — as opposed to its already-instantiated authenticated client object via a shared "runtime data" convention — is not something I can confirm is exposed by `ha-grohe_smarthome` today without inspecting its `__init__.py`/`runtime_data` usage specifically (not yet done — flagged in §9).

**Recommendation for the Phase E vertical slice: skip cloud login entirely.** The fastest, lowest-risk path to "HA → Add Integration → device appears" does not require solving Grohe Cloud OAuth at all — see §8. Cloud login (whichever option) becomes a Phase F+ enhancement once the vertical slice's plumbing is proven.

### 4.1 Concrete step sequence (Phase E scope — no cloud login yet)

```
Add Integration → "Grohe Dial"
  Step 1 "user": Name [Kitchen Dial], Dial host/IP [192.168.x.x], Provisioning token [•••]
      → GET http://<host>/version  (already exists, verifies it's really a grohe_dial)
      → unique_id = dial's MAC (already computable today, see §1.1) → abort if already configured
  Step 2 "credentials" (optional in Phase E): user_id / preshared_key_base64, pasted manually
      → POST http://<host>:8080/provision  (already exists, exactly as designed for this)
  → async_create_entry
  → Device "Grohe Dial — Kitchen" appears, entities per §3
```

Cloud-login-based steps (`Grohe Cloud anmelden → Grohe Blue Geräte abrufen → Grohe Blue auswählen → Dial auswählen → Zuordnung speichern`, exactly as the user's own Section 4 UX describes) become a **later** step, inserted before "credentials," once Fork 2 is resolved — architecturally straightforward to slot in later since the config flow is already step-based.

---

## 5. Information / secrets needed

| Secret/Info | Source | Already available how |
|---|---|---|
| `user_id` (OIDC `sub`) | Grohe Cloud JWT | Manual paste (Phase E) → cloud login (later) |
| `pre_shared_key_base64` | `GET /v3/iot/dashboard` → `appliance.presharedkey` | Manual paste (Phase E) → cloud login (later) |
| Dial's provisioning token | User-configured on the dial (`provisioning_secret_local.hpp`) | Already exists; user must know it (documented in `docs/ARCHITECTURE.md`) |
| Dial's IP/hostname | Local network | Manual entry (Phase E); mDNS is a firmware gap (§2.3), out of scope unless separately approved |
| Grohe Cloud email/password | User's own Grohe account | Only needed once Fork 2 (cloud login) is resolved |
| `appliance_id` / `location_id` / `room_id` | Grohe Cloud `dashboard`/`locations` | Only needed once cloud login exists |

**Storage**: HA `ConfigEntry.data` (encrypted at rest by HA core) for the token/secret fields — never logged, matching this project's own established discipline (`ProvisioningServer` itself never logs credential values, only lengths — same standard applies here). No credentials hardcoded, none in this document beyond field *names*.

---

## 6. Reusable from `grohe_blue_ble`

Given the "HA → Dial → BLE → Blue" architecture (§4, Fork 1), **the BLE transport code (`ble.py`) is not used by the HA integration at all** — the dial does BLE, not HA's own host. What *is* directly reusable:

- **Nothing needs porting for the integration itself** — the protocol/HMAC logic already lives in the firmware (C++, already hardware-verified) and does not need a second Python implementation for this architecture.
- `grohe_blue_ble`'s docs (`REVERSE_ENGINEERING.md`, response-code table, water-type enum) are useful **as a cross-reference / consistency check** when writing the dial's new network API responses (§7), so field names/enums stay consistent across the whole project.
- If Fork 1 is later decided the other way (HA talks BLE directly, bypassing the dial) — not recommended, see §4 — `grohe_blue_ble` would become directly load-bearing (`client.py`'s `GroheBlueClient` is already a clean enough async context-manager API for a HA `DataUpdateCoordinator` to wrap). Flagged for completeness only.

---

## 7. Necessary changes

### 7.1 Firmware (`grohe_blue_dial`) — required for *any* useful control surface

1. **New local status/command HTTP API** (Option B, §4) or **new MQTT command topics** (Option A) exposing: current `BleState`, latest `ApplianceState`, and two new commands wired to the *existing* `GroheClient::RequestDispense()/RequestStop()` — no BLE/protocol logic changes, purely a new caller.
2. **Optional**: per-device BLE targeting using the derived-device-name algorithm (§1.3) if/when multi-appliance disambiguation is wanted — not required for the Phase E vertical slice (today's single-appliance behavior is unchanged and sufficient for one dial + one Blue).

### 7.2 New: `custom_components/grohe_dial/` (or similar) in a Home Assistant config directory — **not this repo**

A standard HA custom integration skeleton: `manifest.json`, `config_flow.py`, `coordinator.py`, `__init__.py`, entity platform files per §3. **This code does not belong in `grohe_blue_dial` at all** — it's a separate Python package with its own repo/HACS listing, exactly like `ha-grohe_smarthome` itself is separate from any firmware repo. This is a genuinely new repository, out of `grohe_blue_dial`'s own git history — flagged explicitly since M15's own git rules (§13 of your message) only make sense for *this* repo's changes (the firmware-side additions in §7.1).

### 7.3 Existing MQTT — unaffected

No deletion, no functional change to `components/dial_mqtt/` in this milestone, per your explicit instruction.

---

## 8. M15 implementation plan (small, checkable steps)

**Phase E — vertical slice** (this repo + a new, separate HA custom-integration repo):
1. *(this repo)* Add the new local status/command HTTP endpoint (§7.1) — diagnostic-only at first (GET status), build+flash+hardware-verify, exactly the established "one change, full test cycle" discipline from M14.
2. *(this repo)* Add the command POST (dispense/stop), wired to existing `GroheClient` methods — hardware-verify against a real Grohe Blue.
3. *(new repo)* Minimal `config_flow.py`: name + host/IP + provisioning token → `GET /version` validation → `unique_id` = dial MAC → `async_create_entry`.
4. *(new repo)* `__init__.py` sets up a `DataUpdateCoordinator` polling the new status endpoint; one `Device` entry appears in HA.

**Phase F** — entities per §3 (connection state, water type, amount, dispense/stop buttons, error state), manual credential entry (user_id/preshared_key, reusing the existing `/provision` endpoint).

**Phase G** — automated tests (config flow success/failure, coordinator polling, command success/failure, invalid credentials, device-unavailable, malformed-response handling) — Python-side (`pytest` + HA's own test harness, `pytest-homeassistant-custom-component`), mirroring `grohe_blue_ble`'s own test discipline; firmware-side additions get the same hardware-test rigor M14 established.

**Phase H** — hardware test against the real dial + real Grohe Blue.

**Deferred, explicitly not in Phase E–H**: Grohe Cloud login (Fork 2), CO₂/filter cloud sensors, multi-appliance BLE disambiguation, MQTT-vs-native transport final decision beyond the Phase E recommendation, any `ha-grohe_smarthome` cross-integration device sharing.

---

## 9. Risks / open questions (explicit UNKNOWNs)

1. **Fork 1** (§4): MQTT-backed vs. new local HTTP transport — recommendation given, not decided.
2. **Fork 2** (§4): own cloud-login implementation vs. `koproductions-code/grohe` dependency vs. cross-integration reuse of `ha-grohe_smarthome`'s own stored auth — the third option's feasibility is **UNKNOWN**, not yet spiked.
3. **Shared device ownership** between this integration and `ha-grohe_smarthome` (§3) — HA's device registry model for this is **UNKNOWN** without a dedicated spike; `via_device` + shared Area is the confirmed-available fallback.
4. **Multi-Grohe-Blue-in-range disambiguation** — algorithm is now evidence-backed (§1.3) but **unimplemented and unverified against real hardware** — the derived-name string has never been computed against a real `applianceSerialNumber` in this project.
5. **`koproductions-code/grohe`'s login flow is a screen-scrape**, not a documented API — a Grohe login-page redesign could silently break it at any time; this risk exists whether we depend on the package directly or port the same technique ourselves.
6. **Simultaneous BLE connections**: whether the Grohe Blue accepts more than one concurrent GATT connection is **UNKNOWN** — irrelevant under the recommended architecture (only the dial ever connects), but would matter if Fork 1 were ever decided the other way.
7. **New firmware HTTP/MQTT command surface's RAM cost** — no measurement exists yet; M14's hard-won headroom (§ M14 in `docs/ROADMAP.md`) must be re-validated once this lands, using the same `mem_diag` checkpoints, not assumed safe.
8. Whether `provisioning_secret_local.hpp`'s token is an acceptable *sole* auth mechanism for a command endpoint that can make the appliance dispense water (vs. the current provisioning endpoint, which can only ever reach `DialSettingsStore`) — a materially higher-stakes endpoint, worth an explicit security-posture review before Phase E step 2 ships, not assumed identical to the existing provisioning threat model.
