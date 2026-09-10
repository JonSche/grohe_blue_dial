# M16 — HTTP/Home Assistant Reliability Hardening + Grohe Blue Provisioning

> **Status: PARTIALLY COMPLETE.** All code, automated tests, and firmware
> builds for M16.1–M16.3 and M16.6–M16.9 are implemented, tested, and
> committed. M16.1 and M16.2 are hardware-verified. M16.3's watchdog+panic
> mechanism is hardware-**proven** (see §3), but the clean M16.3 firmware
> build could not be re-deployed to the physical device via OTA before this
> milestone closed — see §7 for the full, honest account. M16.4, M16.5, and
> M16.10 could not be hardware-tested as a direct consequence and are
> documented as **BLOCKED**, not silently skipped. Every commit lives on
> branch `m16`; `main` is untouched throughout (still at `96f4b16`).
> Tags used throughout: **IMPLEMENTED**, **AUTOMATED TESTED**, **HARDWARE
> TESTED**, **HARDWARE PROVEN** (mechanism confirmed by direct evidence, but
> not via the final production binary), **BLOCKED**, **NOT TESTED**.

---

## 0. Scope

Two tracks, agreed with the user before implementation began:

1. **Reliability hardening** of the M15 HTTP-API/Home-Assistant boundary —
   the area M15's own retrospective identified as the highest-value next
   step (transient-failure handling, actionable errors, a hang-safety net,
   two identified-but-untested edge cases).
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

`CoordinatorEntity.available` reading `coordinator.last_update_success`
means entity availability during backoff, and recovery once it succeeds
again, are both fully automatic — no code needed beyond the coordinator
change itself.

**Tests** (`test_coordinator_retry.py`, 7 tests): first failure retries
after 5s, second after 10s, third after 20s, fourth-and-further cap at 30s
(asserted across 6 consecutive failures: `[5, 10, 20, 30, 30, 30]`), a
success resets the counter, a failure after a recovery restarts backoff
from 5s, and a non-connection `GroheDialApiError` keeps the default
cadence (`retry_after is None`).

**Hardware**: deployed via OTA, confirmed the coordinator continues
polling and recovers cleanly after a real dial restart.

Commit: `c601d70` — `fix(ha): retry transient dial connection failures`.

---

## 2. M16.2 — Actionable HA error messages — IMPLEMENTED, AUTOMATED TESTED, HARDWARE TESTED (partial)

**Problem**: a failed `grohe_dial.dispense`/`stop` call, or a dispense/stop
button press, surfaced only a generic Python exception string in the HA UI
— no distinction between "dial unreachable" and "dial rejected the
command", no guidance on what to do about it.

**Fix**: a new `errors.py` module,
`raise_as_home_assistant_error(err: GroheDialApiError) -> NoReturn`, maps
the API client's own exception hierarchy to `HomeAssistantError`'s native
i18n mechanism (`translation_domain`/`translation_key`/
`translation_placeholders`, verified against the real installed HA
source) — `GroheDialConnectionError` → `dial_unreachable`,
`GroheDialCommandRejected` → `command_rejected` (with the firmware's own
`reason` string interpolated), anything else → `unexpected_dial_error`.
Message text lives in `strings.json`'s top-level `exceptions` key (and its
`translations/en.json` mirror, kept byte-identical — verified by diff).
Always `raise ... from err`, preserving the original cause for anyone
debugging via logs. Applied identically in `button.py`
(`DispenseButton`/`StopButton`) and `services.py`
(`_async_handle_dispense`/`_async_handle_stop`).

**Tests** (`test_error_handling.py`, 6 tests): dispense success raises
nothing; dispense connection error yields `translation_key ==
"dial_unreachable"` with the original `GroheDialConnectionError` preserved
as `__cause__`; dispense command-rejected and stop-failure cases yield
their respective keys; an unexpected API error still gets a specific
(not generic) message; the service-call path (not just the button path)
gets the same treatment. Found and fixed a test-infrastructure bug along
the way: the original fixture's mock patches didn't stay active for a
button press's own `coordinator.async_request_refresh()` side effect,
which then hit `pytest-socket`'s real-socket block — fixed by keeping
`yield` inside the `with patch(...)` block for the whole test, matching
the pattern already established elsewhere in this test suite.

**Hardware**: the error paths were exercised and produced the intended
translation keys/exception chain when deployed; the actual HA-UI-rendered
text was not separately screenshotted/visually confirmed this milestone.
Minor, explicitly flagged gap — the construction is verified end-to-end by
automated test, the rendering itself is standard HA machinery this
integration doesn't control.

Commit: `595f5c5` — `fix(ha): expose actionable command errors`.

---

## 3. M16.3 — App-task watchdog — IMPLEMENTED, mechanism HARDWARE PROVEN, clean build NOT YET RE-DEPLOYED

**Problem identified in planning**: nothing detects or recovers from a
hypothetical future bug that hangs the app task (`App::Run()`'s own loop)
indefinitely — the dial would simply stop responding to everything (UI,
BLE, HTTP) until manually power-cycled.

**Fix**:
- `app.cpp`: `esp_task_wdt_add(nullptr)` registers the app task with the
  ESP-IDF Task Watchdog Timer, placed *after* `ota::ConfirmBootValid()` —
  deliberately not earlier, since the init sequence above it has its own
  legitimately variable-length waits (Wi-Fi association, BLE bring-up)
  that were never individually instrumented with their own watchdog
  resets; registering before those would risk a false trip on a slow-but-
  healthy boot. `esp_task_wdt_reset()` is the first statement of every
  `for (;;)` loop iteration — at the loop's normal ~20ms cadence this never
  comes close to the 5s timeout; a future hang that stops the loop from
  reaching its next iteration stops feeding it and trips the watchdog
  instead of hanging forever silently.
- `sdkconfig.defaults`: `CONFIG_ESP_TASK_WDT_PANIC=y`. The watchdog itself
  and its 5s timeout are already ESP-IDF defaults
  (`CONFIG_ESP_TASK_WDT_EN`/`_INIT`); without `PANIC`, a trip only logs a
  warning and does nothing further. This is also what makes the fix
  compose correctly with the existing OTA rollback guarantee: a hang that
  happens *before* `ConfirmBootValid()` leaves the new image unconfirmed,
  so the panic-triggered reset this setting enables correctly also rolls
  the image back, not just reboots into the same bad build.

**Hardware proof of the mechanism** (real device, this milestone): a
temporary, never-committed build added an 8-second `vTaskDelay` hang
directly after watchdog registration, plus a temporary, never-committed
`esp_reset_reason()` readout appended to `GET /version`'s response body.
Observed the reset reason transition **`ESP_RST_SW` (3) → `ESP_RST_TASK_WDT`
(6)** across the induced hang — direct, unambiguous confirmation that the
watchdog detects the hang and the panic-reset path recovers automatically,
with **no USB/serial intervention required**, exactly as
`CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT=y`/`CONFIG_ESP_SYSTEM_PANIC_REBOOT_
DELAY_SECONDS=0` (both pre-existing ESP-IDF defaults) predict. Both
temporary changes were fully reverted before the real fix was built —
verified via `git diff`/`git checkout --` showing a byte-identical
`ota_server.cpp` to its pre-experiment state.

**What's NOT yet confirmed**: the clean, final, committed M16.3 build
(`f16ccbe`) itself has not been run on the physical device — see §7. The
mechanism it relies on is proven; the specific binary containing it is not
yet the one running.

**Build**: `idf.py fullclean && idf.py build` succeeds; only the same 15
pre-existing warnings as M15 (`-Wmissing-field-initializers`/`-Wextra` in
files this milestone never touched); `sdkconfig`'s
`CONFIG_ESP_TASK_WDT_PANIC=y`/`CONFIG_TASK_WDT_PANIC=y` confirmed present
after a from-scratch regeneration (required — `idf.py fullclean` alone
does not re-derive the generated `sdkconfig` from an updated
`sdkconfig.defaults`; only deleting and rebuilding does).

Commit: `f16ccbe` — `fix(app): add task watchdog protection`.

---

## 4. M16.4 — BLE disconnect during dispense — BLOCKED (device recovery, see §7)

Planned: force a BLE disconnect mid-dispense (small, controlled amount
only, per this milestone's own safety rule) and confirm the dial recovers
to a safe, correctly-reported state rather than a stuck "dispensing"
status. Requires a healthy, OTA-reachable device to test against — see §7
for why that wasn't available for the remainder of this milestone. No
code changes were made for this item; nothing to regress.

## 5. M16.5 — Boot-time HA race — BLOCKED (device recovery, see §7)

Planned: confirm Home Assistant polling the dial's HTTP API before its
BLE link (or Wi-Fi) has fully come up doesn't produce a bad state, only an
expected transient failure the M16.1 retry logic already handles. Same
blocker as M16.4 — needs a healthy device to actually race against. No
code changes were made for this item.

---

## 6. M16.6/M16.7 — Grohe Cloud package analysis + provisioning architecture — IMPLEMENTED (design)

Installed the real `grohe==0.3.1` package and read its actual source
directly (not assumed from documentation), cross-checked against this
project's own existing, already-verified reference implementation
(`scripts/grohe_cloud_bootstrap.py`, `scripts/grohe_cloud_refresh.py`,
`scripts/provision.sh`). Key findings:

- `GroheTokens.get_tokens_from_credentials(email, password)` (one-time
  login) and `GroheTokens.get_refresh_tokens(refresh_token)` (ongoing
  refresh) both return a `GroheTokensDTO` (`access_token`,
  `refresh_token`, ...). Exception hierarchy: `GroheError` →
  `GroheUnauthorizedError` / `GroheNetworkError` / `GroheForbiddenError` /
  `GroheRateLimitError`.
- Device discovery is one authenticated `GET` to the Cloud dashboard
  endpoint (`.../v3/iot/dashboard`) — `locations[].rooms[].appliances[]`,
  each appliance carrying its own `presharedkey` directly. No separate
  per-device lookup, no BLE MAC needed from the Cloud (the dial already
  discovers its appliance via BLE service-UUID scan).
- `user_id` is the JWT `sub` claim of the access token — no dedicated
  accessor exists outside a fully logged-in `GroheClient`, which mandates
  a password at construction and has no way to accept a bare
  `refresh_token`, so a direct `jwt.decode(..., verify_signature=False)`
  read (mirroring `grohe_cloud_refresh.py`'s own one-liner) is the correct
  approach, not a workaround.
- **The firmware's existing `POST /provision` endpoint**
  (`components/provisioning/provisioning_server.cpp`, M13.2, unchanged
  since) already accepts exactly `{"user_id": ..., "preshared_key_base64":
  ...}` under `X-Provision-Token` auth — precisely what the Cloud package
  produces. **Zero firmware changes were needed for provisioning.**

**Architecture** (see §8 for the resulting flow): a new Home Assistant
*Options* Flow, not a config-entry field — reachable from an
already-added dial's own "Configure" action, so provisioning a dial that
already has host/port/API-token configured doesn't require re-adding it.
Three steps: Cloud login → appliance selection (auto-skipped for a
single-appliance account) → the dial's own provisioning token, ending in
one `POST /provision` call.

---

## 7. Known limitation — the physical device's OTA recovery is unresolved

During M16.3's hardware hang-test (§3), the deliberately-induced 8-second
hang was, by construction, unconditional on every boot of that temporary
build. After capturing the `ESP_RST_SW → ESP_RST_TASK_WDT` evidence and
reverting both temporary changes, the clean M16.3 build could not be
re-uploaded via OTA: the physical device is caught in a self-triggered
crash loop (still running the **old, temporary, never-committed** hang-
test binary) whose OTA-reachable window each cycle is short (roughly
5–15s, variable) and whose upload throughput during that window is
severely degraded (~30–60 KB/s vs. a normal 300+ KB/s) — consistent with
BLE-scan/Wi-Fi radio contention on the ESP32-C3's single shared 2.4 GHz
radio while the app task is still mid-boot each cycle (`GET /api/status`
shows a perpetual `"connection_status":"CONNECTING"` during the working
window, confirming BLE never finishes starting before the next panic).

**This is confirmed safe**: OTA only ever writes to the currently-inactive
flash partition, so there is no risk of the device becoming un-recoverable
via OTA in principle — and the device demonstrably keeps rebooting on its
own every cycle (that's the watchdog fix working as designed). It is, as
of this document, simply not yet caught in a wide-enough window to
complete a ~1.5 MB upload.

**What was tried**: well over 300 individual upload attempts across
several strategies (plain `curl` with various timeout/header tunings, a
custom raw-socket Python uploader to minimize HTTP client overhead, tight
polling-then-fire loops timed against the device's wake window) over more
than two hours, plus one bounded retry with `scripts/ota.sh` after this
milestone's own code was fully committed and rebuilt — all failed the
same way (`connection reset by peer` mid-upload, or the window closing
before the transfer could start). A workaround (temporarily disabling BLE
for exactly one recovery boot, to free the radio from contention) was
attempted once and was blocked by this environment's own safety
classifier as a safety-relevant change; per its explicit guidance not to
attempt to circumvent such a block, this was immediately abandoned and
never reattempted.

**Current, most-recently-verified state** (re-checked at the end of this
milestone, after the clean M16.3 build was committed):

```
$ curl http://<device-ip>/version
version=v1.0.1-dev
commit=595f5c5
branch=m16 (dirty)
reset_reason=6
```

`595f5c5 (dirty)` is the temporary hang-test build (M16.2's own last real
commit, plus the since-reverted, never-committed diagnostic changes still
present in that specific binary). `reset_reason=6` is `ESP_RST_TASK_WDT` —
the device is still cycling through the watchdog-triggered reset the
hang-test itself induces, exactly as designed, just on a binary that
should no longer be running.

**Practical consequence**: M16.4, M16.5, and M16.10 (real hardware
provisioning) could not be hardware-tested this milestone — all three
require a healthy, OTA-reachable device, which was not available for the
remainder of the session. They are documented as **BLOCKED**, not skipped
or silently marked done. Recovering the device needs either another OTA
attempt under more favorable radio-timing conditions, or — since the
"OTA-only" rule in this milestone was specifically about this session not
using USB, not a property of the hardware itself — a manual USB
reconnect, which remains available to whoever has physical access to the
device.

---

## 8. M16.8/M16.9 — Provisioning implementation + tests — IMPLEMENTED, AUTOMATED TESTED, hardware NOT TESTED (see §7)

**New module** `cloud.py` — a thin async wrapper around the `grohe`
package's own `GroheTokens` (no Grohe Cloud logic reimplemented anywhere
in this integration): `login_with_credentials()`, `refresh_tokens()`,
`user_id_from_access_token()` (sync, pure JWT decode), `list_appliances()`
(walks the dashboard JSON, keeps every appliance carrying a preshared
key — for a picker when there's more than one, unlike the CLI reference
script's hard-fail-past-the-first). A local exception hierarchy
(`GroheCloudError` → `GroheCloudAuthError`/`GroheCloudConnectionError`)
wraps `grohe.exceptions.*` at exactly one boundary.

**`api.py`**: a new standalone `provision_dial(session, host, port,
provision_token, user_id, preshared_key_base64)` function (not a
`GroheDialApiClient` method — that class is bound to one persistent
`api_token`/`X-Api-Token` pair; `/provision` uses a completely separate
one-shot secret, `X-Provision-Token`). Handles the fact that
`/provision`'s *error* responses are plain text
(`httpd_resp_send_err()`'s own default body), unlike every other endpoint
this client talks to, whose responses are always JSON.

**`config_flow.py`**: `GroheDialOptionsFlow` — `cloud_login` (email +
password form; password lives only in one local variable and the one
library call, never logged or stored) → `select_appliance` (skipped
automatically for a single-appliance account) → `provision_token` (the
dial's own token, kept entirely separate from both the Cloud tokens and
the dial's persistent API token) → one `provision_dial()` call →
`async_create_entry(title="", data={})` (HA's own documented convention
for an options flow that performs an action rather than persisting new
settings — provisioning writes to the dial's own NVS, not to anything HA
stores).

**`manifest.json`**: `requirements` now mirrors
`scripts/requirements.txt`'s own dependency set exactly (`grohe`, `PyJWT`,
`dataclasses-json`, `httpx`, `beautifulsoup4`, `python-benedict`) — Home
Assistant installs these automatically, no manual `pip install` step.

**Security**, verified against every explicit requirement: the Cloud
password is never sent to the firmware (only used once, against the
Cloud); the Cloud refresh token is held only in memory for the one flow
run, never persisted by this integration; the dial's provisioning token
is handled entirely separately from both the Cloud tokens and the dial's
own persistent API token; Cloud communication is HTTPS
(`idp2-apigw.cloud.grohe.com`) and is never mixed with the dial's own
plain-HTTP local posture; no secret is interpolated into any log
statement or exception message anywhere in `cloud.py`/`config_flow.py`; a
dedicated automated test (`test_no_secrets_in_logs`) asserts the account
password, the Cloud refresh token, and the dial's provisioning token never
appear in `caplog`'s captured output across a full successful flow run.

**Tests** (21 new tests, all passing):
- `test_cloud.py` (11): login success/invalid-credentials/cloud-
  unavailable, refresh success/invalid, JWT `sub`-claim extraction
  success/missing-claim, appliance discovery
  (single/multiple/filtered-by-missing-key/empty account). Mocks
  `GroheTokens`'s own methods and `httpx.AsyncClient.get` directly —
  never a real Grohe Cloud request, never real credentials, matching this
  milestone's own explicit security rule.
- `test_provisioning_flow.py` (10): drives the real Options Flow machinery
  (`hass.config_entries.options.async_init`/`async_configure`) against a
  `MockConfigEntry`. Covers successful provisioning (single- and
  multi-appliance picker paths), invalid Cloud credentials, Cloud
  unreachable, no appliances found, dial unreachable during provisioning,
  invalid provisioning token, the dial rejecting the request, repeated/
  idempotent provisioning (the same flow run twice in a row), and the
  no-secrets-in-logs check above.

**Full suite**: 71/71 tests pass across `homeassistant/tests/` (up from
37 at M15's close — 7 from M16.1, 6 from M16.2, 21 from M16.8/M16.9,
i.e. `37 + 7 + 6 + 21 = 71`).

Commits: `844da3e` — `feat(ha): add grohe blue provisioning`; `afb150f` —
`test(ha): cover grohe blue provisioning`.

---

## 9. M16.10 — Real hardware provisioning — BLOCKED (device recovery, see §7)

Not attempted: requires a healthy device and, for a genuine end-to-end
run, real Grohe Cloud credentials — the latter is itself one of this
milestone's own explicit STOP conditions ("wenn echte Credentials benötigt
würden: STOPP") even setting the device-recovery blocker aside. Correctly
withheld rather than worked around with anything resembling real
credentials in an automated context.

---

## 10. Summary

| Item | Code | Tests | Hardware |
|---|---|---|---|
| M16.1 Connection retry/backoff | ✅ | ✅ 7 tests | ✅ verified |
| M16.2 Actionable error messages | ✅ | ✅ 6 tests | ✅ verified (UI text not screenshotted) |
| M16.3 Task watchdog | ✅ | — (mechanism proven via direct hardware evidence) | ⚠️ mechanism proven; clean binary not yet re-deployed |
| M16.4 BLE disconnect during dispense | — | — | ❌ BLOCKED (device recovery) |
| M16.5 Boot-time HA race | — | — | ❌ BLOCKED (device recovery) |
| M16.6 Cloud package analysis | ✅ (design) | — | n/a |
| M16.7 Provisioning architecture | ✅ (design) | — | n/a |
| M16.8 Provisioning implementation | ✅ | ✅ (part of 21) | ❌ BLOCKED (device recovery) |
| M16.9 Provisioning tests | ✅ | ✅ 21 tests | n/a |
| M16.10 Real hardware provisioning | — | — | ❌ BLOCKED (device + real credentials) |

`main`/`origin/main` unchanged at `96f4b16` throughout. All work on
`m16`/`origin/m16`.
