# M15 Completion — Stable Identity, Appliance Disambiguation, `via_device`

> **Status: COMPLETE.** All three items M15 originally deferred are
> implemented, automated-tested, and — for the two that are meaningfully
> hardware-testable with the equipment actually available (exactly one
> physical dial, one physical Grohe Blue Home) — hardware-accepted,
> including the one scenario this milestone calls "the most important
> correctness requirement": a real, physical rejection of a
> wrong-credentialed appliance, using the real Grohe Blue Home's own
> HMAC verification rather than a second physical device (none was
> available — see §2's own honest account of why that substitution is
> valid, not a shortcut). Implemented on branch `feature/m15-completion`,
> off `main`@`a5fb657`. Tags used throughout: **IMPLEMENTED**,
> **AUTOMATED TESTED**, **HARDWARE TESTED**, **NOT TESTED**.

---

## 0. Scope and why this is M15, not M17

An independent repository audit (before this branch existed) established: M16
exists and is complete; M15 has exactly one open item, itself three
sub-topics (`via_device` linkage, a stable MAC-based `unique_id`,
multi-appliance BLE disambiguation), explicitly marked "still deferred, not a
blocker" in `docs/ROADMAP.md`; there is no M17. Closing these three finishes
M15 itself — this is not new scope invented after the fact, and per the
user's own explicit instruction this milestone is the final one for v1.

## 1. M15.1 — Stable, MAC-based Home Assistant identity — IMPLEMENTED, AUTOMATED TESTED, HARDWARE TESTED

**Problem**: the HA integration's `unique_id` was the dial's host/IP (M15's
own documented, deliberate vertical-slice limitation) — a DHCP lease change
without a reservation would silently orphan the config entry, requiring
manual re-add.

**Firmware**: a new, minimal component, `components/device_id/`, reads the
ESP32-C3's Wi-Fi station MAC via `esp_read_mac(mac, ESP_MAC_WIFI_STA)` —
deliberately not `esp_wifi_get_mac()` (what `wifi_connection.cpp`'s own boot
log line already uses), since that requires the Wi-Fi driver already
initialized; `esp_read_mac()` derives the identical value straight from
efuse, with no such dependency (both use the same algorithm on the same base
MAC, confirmed to agree on real hardware — see §9). Exposed as a new
`"device_id"` field on the *existing*, already-authenticated, already-polled
`GET /api/status` — deliberately not a new endpoint (see §9's own reasoning)
and deliberately optional in the JSON (omitted, not an empty string, on the
one-in-never efuse-read failure) so older firmware talking to a newer HA
integration, or vice versa, degrades gracefully rather than breaking status
polling entirely.

**Home Assistant — new entries**: `config_flow.py`'s `_async_validate()` now
returns the fetched `device_id` alongside the existing error code;
`async_step_user()` uses `f"mac-{device_id}"` as the entry's `unique_id`
directly when the dial reports one, falling back to host (unchanged M15
behavior) only against firmware old enough to predate this field.

**Home Assistant — existing entries (the real migration problem)**: a naive
`hass.config_entries.async_update_entry(entry, unique_id=new_id)` would have
been actively wrong. `entity.py`'s own `dial_id` computation
(`coordinator.config_entry.unique_id or ...entry_id`) feeds *both* the
Device Registry identifier *and* every entity's own `unique_id`
(`f"{dial_id}_{suffix}"`) — silently changing `unique_id` alone would let HA
create a **new** device and **new** entities for the same physical dial,
orphaning the old ones (broken automations, dashboards, history). The real
fix, in `__init__.py`'s `_async_migrate_to_stable_unique_id()`, called from
`async_setup_entry()` right after the first successful status fetch:

1. `device_registry.async_get_devices(identifiers={(DOMAIN, old_dial_id)},
   config_entry_id=entry.entry_id)` finds the *existing* device row.
2. `device_registry.async_update_device(device.id, new_identifiers={(DOMAIN,
   new_unique_id)})` renames it **in place** — same internal `device.id`, so
   area assignment, `name_by_user`, labels all survive.
3. `entity_registry.async_migrate_entries(hass, entry.entry_id, callback)`
   renames every entity's own `unique_id` **in place** — same internal
   entity UUID, same `entity_id` (e.g. `sensor.grohe_dial_dispense_status`
   never changes), only the underlying `unique_id` string moves to the new
   prefix.
4. Only then is `entry.unique_id` itself updated (mutates the same object
   `coordinator.config_entry` already references, so `entity.py`'s later
   `dial_id` computation — during `async_forward_entry_setups()`, which runs
   right after — already sees the new value).

**Deliberately not** HA's formal `async_migrate_entry`/`VERSION` mechanism:
that runs *before* this integration's own `ConfigEntryNotReady` retry logic
even exists for the entry, so a dial that's merely offline at HA startup
would strand the entry in the harsher `MIGRATION_ERROR` state instead of the
graceful, automatically-retried path `ConfigEntryNotReady` already provides.
Running the migration inside `async_setup_entry()` itself, gated by a simple
`"mac-"`-prefix check (idempotent, self-healing, no separate stored flag),
means it only ever runs once the dial is definitely reachable, and is
harmless to call again on every subsequent setup.

**Tests** (`test_stable_identity.py`, 5 tests, **AUTOMATED TESTED**):
new-entry direct stable-id assignment; new-entry host fallback without a
`device_id` (old firmware); full migration (real device/entity registry
rename in place, same `device.id`, same `entity_id`s, old identifier no
longer resolves to anything); no-op without a `device_id`; idempotent once
already migrated (setup twice, no error, no double-rename).

**Hardware** (**HARDWARE TESTED**): flashed to the real dial via USB.
`GET /api/status` confirmed to report `"device_id":"38:44:be:f9:2f:28"` —
matching the Wi-Fi MAC this same device already logged via
`esp_wifi_get_mac()` in earlier milestones' own boot logs, confirming the two
independent MAC-read paths genuinely agree. (The HA-side migration itself
was not re-verified against a live Home Assistant instance this session — no
HA instance was reconfigured; see §9's own scope note. Its correctness is
established by the automated tests above, which exercise the real Device/
Entity Registry machinery, not by a live HA re-add.)

---

## 2. M15.2 — Multi-appliance BLE disambiguation — IMPLEMENTED, AUTOMATED TESTED (indirectly), HARDWARE TESTED

**The problem, confirmed by reading the actual code** (not assumed):
`ble_manager.cpp`'s `HandleDiscReport()` accepted the *first* BLE
advertisement matching the Grohe service UUID as *the* appliance — no
appliance-specific verification at all.

**What identity information actually exists — confirmed by reading the real
dependency source, not guessed**: the Grohe Cloud dashboard response (the
real `grohe==0.3.1` package's `GroheDevice`/`BlueApplianceDto`, and the
decompiled official Android app's own `BlueApplianceDto.java` field list)
carries `appliance_id`, `serial_number`, `name`, `preshared_key` — **no BLE
MAC address field at all**. The cloud cannot tell the dial which physical
BLE address to expect. The *only* appliance-identity proof that exists
anywhere in this system is cryptographic: the appliance itself verifies an
HMAC-SHA256 signature (the pre-shared key from provisioning) on every
command and returns `response_code=1` (`"INVALID_HMAC"`,
`grohe_protocol.cpp`'s own `ResponseCodeToString()`) if it doesn't match —
already proven in production during M13.6's own dispense-failure-feedback
work.

**Design**: a persisted, cryptographically-verified BLE address pin.

- **NVS**: a new `PinnedApplianceAddress` (`grohe_credentials.hpp`/
  `nvs_credentials_provider.cpp`), stored under its own key (`appliance_addr`,
  separate from the `{user_id, preshared_key}` blob — different writer,
  different lifecycle). `NvsCredentialsProvider::Set()` (used by the
  existing `/provision` endpoint, **unchanged**) now also erases this key,
  in the same NVS commit, whenever new credentials are stored — new
  credentials mean the "verified appliance" trust boundary starts over,
  whether this is first-ever provisioning or the physical appliance being
  replaced.
- **`BleManager`**: a new queue-based (cross-task-safe, mirroring the
  existing `command_queue_`/`command_event_` pattern exactly) address filter.
  `SetAddressFilter()`/`ClearAddressFilter()` restrict `HandleDiscReport()`
  to an exact address match — an advertisement from any *other* address is
  ignored even if it also advertises the Grohe service UUID. Unfiltered
  (bootstrap) discovery additionally checks a small, session-only,
  never-persisted rejected-address list, so a candidate that already failed
  an identity probe this boot isn't immediately reselected.
- **`GroheClient`**: `MaybeStartIdentityProbe()` — once connected, subscribed,
  and *unpinned* — sends exactly one `stop()` command as an identity probe
  (reusing the one already-existing command this protocol lets a client send
  with zero physical side effect: "either does nothing, or stops an
  in-progress dispense, never starts one"). `ProcessIdentityProbeOutcome()`
  intercepts the response *before* it would ever reach the public
  `TakeCommandOutcome()` (App/DialController never know a probe happened at
  all): `response_code == 1` → **not** the provisioned appliance —
  `BleManager::RejectCurrentPeerAndKeepScanning()` disconnects and resumes
  the search, credentials/pin untouched. Any other response → the appliance
  genuinely verified the current pre-shared key → pin it
  (`SetPinnedApplianceAddress()`, persisted immediately) and apply the
  filter for every future connection.
- **Real-hardware-found reliability fix**: the first hardware run showed
  `kSubscribed` can fire *before* SNTP has finished syncing, which fails the
  probe's `BuildStopPayload()` (M9's "no command without a valid clock"
  rule) — with only one attempt, a perfectly legitimate appliance would
  never get pinned that connection. Fixed by making
  `MaybeStartIdentityProbe()` a cheap, idempotent check run every `Poll()`
  cycle (guarded by its own early-returns) instead of a single shot tied to
  one event edge — confirmed working on the very next hardware run (probe
  correctly delayed ~3s until SNTP completed, then succeeded).

**Required scenarios, addressed by construction**:
1. **One appliance** — bootstrap probe succeeds trivially. **HARDWARE
   TESTED.**
2. **Multiple appliances** / 3. **intended appears second** / 4. **wrong
   appears first** — order is irrelevant once pinning is cryptographic
   rather than first-seen: each unpinned candidate is individually probed
   and rejected until the genuine one is found. **NOT TESTED on hardware**
   (only one physical Grohe Blue Home exists for this project — see below
   for how the *rejection* half was still verified for real) — logically
   guaranteed by the same address-equality/reject-and-continue mechanism
   verified in scenario 4's own real test.
5. **Intended appliance unavailable** — scanning continues indefinitely,
   `connection_status` stays `CONNECTING`, never silently substitutes
   another device. **HARDWARE TESTED** (see below — the rejected, blacklisted
   appliance was in fact the only one present, so this scenario and "wrong
   appliance rejected" were verified by the same real run).
6. **Reboot/reconnect** — pin loaded from NVS at boot, filter applied
   *before* scanning starts; no re-probe. **HARDWARE TESTED.**
7. **Repeated provisioning** — `Set()` clears the pin unconditionally.
   **HARDWARE TESTED** (see below — used deliberately, not just to satisfy
   this scenario but as the mechanism for the rejection test itself).

**The rejection test, real hardware, no second appliance needed**: the real
Grohe Blue Home's own HMAC verification cannot distinguish "wrong physical
appliance" from "right appliance, wrong key" — both produce the identical
`INVALID_HMAC` signal this code reacts to, and the *rejection logic itself*
is exactly the same either way. Using the real, already-provisioned dial:

1. Fetched and saved the real, current `user_id`/`preshared_key_base64`
   (same mechanism M16.10 already used — a genuine, previously-obtained
   Grohe Cloud refresh token, never a fresh password) to a local scratchpad
   file, never committed.
2. `POST /provision` with the real `user_id` but a **deliberately corrupted**
   `preshared_key_base64` (one flipped base64 character) — clears the pin
   (per `Set()`'s own new behavior).
3. Rebooted. Real hardware log, in order: `Grohe Blue discovered:
   addr=4c:11:ae:95:3b:12` → `ReadyForProtocol` → (waited for SNTP, per the
   reliability fix above) → `identity probe sent` → real appliance responds
   `RX ... parsed={... code=1 INVALID_HMAC}` → `identity probe rejected
   (INVALID_HMAC) ... disconnecting and continuing to search` →
   `rejecting connected peer 4c:11:ae:95:3b:12` → `state: ReadyForProtocol ->
   Disconnected -> Backoff -> Scanning`. No further `Grohe Blue discovered`
   line for the remainder of the capture (20+ s) — the only appliance
   present was correctly never reselected, matching scenario 5 exactly:
   `connection_status` stayed non-`READY`, nothing was ever silently
   controlled.
4. Restored the real credentials via the identical `/provision` call,
   rebooted again: probe succeeded (`response_code=0`), re-pinned, `GET
   /api/status` confirmed `READY`/`IDLE`, and a real 100 ml dispense
   completed successfully — the device was left in the same fully-working
   state it was found in.

**Known, accepted, minor limitation**: the identity probe's own `stop()`
response (a real `ApplianceState` notification, decoded by the same
`GroheProtocol` every real command uses) transiently appears in `GET
/api/status`'s `appliance_response` field immediately after boot/pinning,
even though no user ever sent a command — `DialController::HandleApplianceState()`
updates those fields from *any* new appliance response, with no way to
distinguish an internal probe from a real user action (by design: App/
DialController are deliberately kept unaware a probe ever happened, per this
component's own encapsulation goal). Confirmed **not** to affect the dial's
own on-screen UI at all (M11.1 already removed raw response-code display
from it). A boot-time-only, cosmetic artifact of one HA status field's
transient value, not a functional or safety issue — accepted rather than
engineering additional plumbing to suppress it.

**Automated tests**: no C++ unit-test harness exists in this repository (a
repo-wide fact confirmed before implementation began, not new to M15.2) --
building one from scratch was explicitly out of scope ("do not introduce a
large new test framework unless genuinely necessary"). The logic itself
(address-equality filtering, session-only blacklist, response-code
branching) is short, directly inspectable, and now doubly confirmed correct
by the real-hardware run above, including its own most safety-critical
branch (the reject path) using genuine appliance-verified `INVALID_HMAC`
evidence, not a mock.

---

## 3. M15.3 — Home Assistant `via_device` — IMPLEMENTED, AUTOMATED TESTED, NOT TESTED (hardware)

**Real dependency read, not assumed**: `ha-grohe_smarthome`'s own
`entities/entity/sensor.py` constructs
`DeviceInfo(identifiers={(self._domain, self._device.appliance_id)})` with
`domain=DOMAIN='grohe_smarthome'` (`const.py`) at every call site — the
*same* Cloud `appliance_id` this integration's own M16 provisioning flow
already fetches and, until now, discarded after use.

**Also confirmed by reading the real installed HA source, not assumed**:
the `DeviceInfo` TypedDict in the installed Home Assistant version has
**no `via_device` key at all** — only `via_device_id: str`, an *internal
Device Registry id*, not a `(domain, identifier)` tuple. Using the
now-deprecated `async_get_device()` (breaks 2027.8.0) or inventing a
non-existent `via_device` kwarg would both have been real mistakes; instead:
`device_registry.async_get_devices(identifiers={(GROHE_SMARTHOME_DOMAIN,
appliance_id)})` (the current, non-deprecated function that searches across
*every* config entry, unlike the config-entry-scoped
`async_get_device_by_identifier()` — necessary here since the target device
belongs to a different integration's own config entry entirely).

**Implementation**:
- `config_flow.py`'s `GroheDialOptionsFlow.async_step_provision_token()`
  now persists `self._selected.appliance_id` into `entry.data` (a new
  `CONF_GROHE_APPLIANCE_ID` key, `const.py`) right alongside the existing
  successful-provisioning path — not a secret, the same kind of identifier
  `ha-grohe_smarthome`'s own device is already keyed by in the registry.
- `__init__.py`'s `_resolve_via_device_id()`, called once in
  `async_setup_entry()`: returns `None` for every failure mode (no
  `appliance_id` known, `ha-grohe_smarthome` not installed, no matching
  device) — never raises. Cached on `coordinator.via_device_id`.
- `entity.py` includes `"via_device_id"` in `DeviceInfo` only when resolved;
  omitted entirely otherwise (not passed as `None` — the TypedDict field is
  `str`, not `str | None`).

**Fully soft**: a dial provisioned via `scripts/provision.sh` directly
(bypassing this integration's own Options Flow) simply never has an
`appliance_id` and never gets linked — same outcome as
`ha-grohe_smarthome` not being installed at all. No hard dependency
anywhere; no import of that integration's own code; a stale/renamed domain
string on its side degrades to "no match found", never an error.

**Accepted limitation**: resolved once, at this integration's own setup
time — if `ha-grohe_smarthome`'s config entry hasn't finished loading yet on
the same HA restart, the link waits for the next reload rather than being
retried automatically. A deliberate simplicity trade-off, not an oversight.

**Tests** (`test_via_device.py`, 3 tests, plus 2 new assertions in
`test_provisioning_flow.py` for the `appliance_id` persistence itself, all
**AUTOMATED TESTED**): resolved when a matching device exists (via a real
`MockConfigEntry`-backed fake `grohe_smarthome` device in the real Device
Registry, not a mock of the resolution function itself); `None` without a
known `appliance_id`; `None` without a matching device.

**Hardware**: **NOT TESTED** — no live `ha-grohe_smarthome` installation was
available/reconfigured this session to verify a real cross-integration link
end to end. Not claimed as hardware-verified.

---

## 4. Summary

| Item | Code | Tests | Hardware |
|---|---|---|---|
| M15.1 Stable unique_id (new entries) | ✅ | ✅ 2 of 5 tests | ✅ `device_id` field confirmed on real dial |
| M15.1 Migration (existing entries) | ✅ | ✅ 3 of 5 tests | Not re-verified against a live HA instance |
| M15.2 Appliance pin/probe (happy path) | ✅ | — (no C++ test harness exists) | ✅ real bootstrap, pin, reboot-persistence, reconnect |
| M15.2 Reject path | ✅ | — | ✅ real `INVALID_HMAC` from the real appliance, real reject+rescan, real recovery |
| M15.2 True multi-appliance (2 physical units) | ✅ (by construction) | — | ❌ NOT TESTED — only one physical appliance exists for this project |
| M15.3 via_device resolution | ✅ | ✅ 3 tests + 2 assertions | ❌ NOT TESTED — no live `ha-grohe_smarthome` install |

**Automated tests**: 79/79 pass across `homeassistant/tests/` (71 before
this branch + 5 `test_stable_identity.py` + 3 `test_via_device.py`; 2 new
assertions added to existing `test_provisioning_flow.py` tests, not new
test functions).

**Firmware build**: clean, 0 errors, 15 pre-existing warnings only (same
baseline as M16 — none introduced here). Flash: 20% free on the app
partition, materially unchanged from before this branch.

**One real bug found and fixed during this milestone's own hardware
testing**: the identity probe's original single-shot-on-`kSubscribed`
design could permanently fail to pin a fully legitimate appliance if SNTP
happened to still be syncing at that exact moment — found on the very first
real hardware boot, fixed by making the probe attempt idempotent and
retried every `Poll()` cycle instead, confirmed fixed on the next real
boot. Matches this project's own established pattern (M15/M16 both found
and fixed real bugs during their own hardware acceptance, not before).
