# Grohe Dial — Dispense Failure Feedback: Design Study

A design study for one missing interaction: what the dial shows when a
dispense command is sent over BLE and rejected, times out, or otherwise
fails.

Nothing here was implemented at the time this document was written. This
document exists to choose a direction before any code is written —
mirroring exactly how
[`dispense_animation_concepts.md`](dispense_animation_concepts.md) preceded
the dispense UI itself before M11.

> **Implementation status (M13.6):** Concept D below was built exactly as
> recommended, and hardware-verified with deliberately wrong BLE
> credentials — see `docs/ROADMAP.md`'s M13.6 section. One detail shipped
> differently from this document's original text: the `hint_label_` copy
> is **`"Bad credentials"`**, not `"Try again"`. The reasoning in §4 for
> *why* a single generic string is enough still holds — this remains the
> generic `kFailed` state with no per-`response_code` classification (see
> §5) — only the specific wording changed, after hardware testing showed
> wrong credentials is, in practice, the rejection this milestone actually
> surfaces. Every other `"Try again"` mention below is the original,
> superseded proposal, left as-is for the historical record.

Visual mockups referenced throughout: **[Grohe Dial — Dispense Failure
Feedback (Artifact)](https://claude.ai/code/artifact/de6b6724-2e25-436e-a983-471207ad7ca6)**.

---

## 0. How this was found

During M13.1/M13.2 hardware validation, `/provision` was used to store
deliberately invalid test credentials (`user_id="abc-123"`,
`preshared_key_base64="c29tZWJhc2U2NA=="`) — provisioning itself worked
exactly as designed, returning `200 OK`. The subsequent dispense attempt
then failed exactly as the BLE protocol says it should (the appliance
replies `INVALID_HMAC`) — but **the display showed nothing at all**: no
water ran, the ring didn't change, no text appeared, and the connection dot
stayed solid blue throughout. For a standalone product with no companion
app and no serial monitor, that's a real gap, not a cosmetic one — the user
has no way to tell "it's still thinking" from "it already failed and gave
up."

---

## 1. How dispensing works today

### 1.1 Where a dispense starts

A short press while idle: `DialController::HandleEvent()`
(`dial_controller.cpp:92-94`) returns `DialAction::kRequestDispense`.
`App::Run()`'s encoder callback (`app.cpp:131-136`) turns that into
`GroheClient::RequestDispense(amount_ml, taste)`, then reports whether it
was *accepted* back via `DialController::HandleCommandSent(accepted)`.

**"Accepted" here is a narrow, specific claim** — `GroheClient::
SendCommand()` (`grohe_client.cpp:68-114`) builds an HMAC-signed payload
(`BuildDispensePayload()`, using whatever `CredentialsProvider::Get()`
currently returns — M13.1's injected seam, `LocalCredentialsProvider` or
`NvsCredentialsProvider`) and queues it as a BLE GATT write
(`BleManager::WriteCharacteristic()`). `accepted == true` means only "the
write was successfully queued onto the BLE stack" — it says nothing about
whether the *appliance* will accept the command once it actually decodes
and verifies the HMAC. Wrong credentials sign a perfectly well-formed,
successfully-transmitted, cryptographically-wrong payload; the write
itself succeeds every time.

### 1.2 Where the result comes back

The appliance's decoded reply becomes `grohe_ble::ApplianceState` (via
`GroheProtocol::HandleNotification()`), which `GroheClient::
TakeCommandOutcome()` surfaces as a `CommandOutcome { available,
was_dispense, state }` once per command. `App::Run()` feeds this to
`DialController::HandleCommandOutcome()` every poll.

**This is the exact point the result is discarded today**
(`dial_controller.cpp:146-172`):

```cpp
if (!outcome.state.is_success) {
  ESP_LOGW(kTag, "%s command rejected: code=%ld", ...);
  if (!outcome.was_dispense && state_.dispense_status == kStopping) {
    state_.dispense_status = kDispensing;  // stop-rejection revert
    return true;
  }
  // A rejected dispense request leaves dispense_status exactly as it
  // was (still Idle) -- nothing to revert.
  return false;
}
```

A rejected **dispense** (as opposed to a rejected *stop*, which already has
a revert path) hits the `return false` at the very end — logged, and
nothing else. `state_changed` never becomes `true` because of it, so
`UiManager::Render()` is never even asked to draw anything different.

### 1.3 Why the UI doesn't already show this

It used to, partially. `docs/ARCHITECTURE.md`'s ["Status text polish
(M11.1)"](../ARCHITECTURE.md#status-text-polish-m111) section documents a
deliberate removal: M11 had a second label showing the raw protocol
response (`"APPL OK"` / `"APPL INVALID_HMAC"` / `"APPL CODE 7"`), and M11.1
removed it entirely — "internal codes useful during M6–M9's
reverse-engineering, never meant for a production UI." That reasoning is
still correct: raw protocol codes don't belong on this screen. But nothing
replaced it with a *production-appropriate* signal, so the M11.1 cleanup
left today's gap as a side effect, not a new decision.

`dial_state::DialState` (`dial_state.hpp:88-96`) already carries
`appliance_response_received`/`appliance_response_success`/
`appliance_response_code` — `HandleApplianceState()` still populates them
on every response — but nothing in `UiManager::Render()` reads them; they
exist purely for the `ESP_LOGI`/`ESP_LOGW` lines right below.

### 1.4 Existing UI states and animations (`ui_manager.cpp`)

The "Interior Crown" connectivity glyphs (§11 of the frozen dispense-UI
spec) and the ring both already have an established "error-adjacent"
idiom: **`ConnectionStatus::kConnectionLost`** desaturates the ring
(`kDesaturatedColor`, `#6B8494`) and shows the hollow connection dot with
`hint_label_` reading `"Connection lost"` — the *only* precedent for
"something is wrong" this UI has ever shipped. Per the frozen spec's own
micro-interaction table: *"Error (Connection Lost / No Time): accent
desaturates — no red, no flashing, no alarm."* That's a real, deliberate
house rule, not an omission — any new failure treatment should read as an
extension of it, not a departure.

The `Finished` state's mechanism is the other relevant precedent: the
numeral (`amount_label_`) and a `checkmark_label_` (`LV_SYMBOL_OK`) are
two objects in the same screen position, toggled via
`LV_OBJ_FLAG_HIDDEN` (`ui_manager.cpp:264-269`), held for
`kFinishedHoldUs` (400 ms), then reverted automatically. No new widget,
no new screen, no navigation.

### 1.5 The BLE indicator, precisely

`connection_glyph_dot_`'s solid/hollow/halo state (`ui_manager.cpp:380-403`)
is driven **exclusively** by `dial_state::ConnectionStatus`, which
`DialController` computes from exactly two BLE lifecycle flags:
`ble_ready_for_protocol_` and `ble_subscribed_` (`dial_controller.cpp:
310-322`) — i.e. *is the GATT link up and subscribed to notifications*.
It has no dependency whatsoever on `CommandOutcome`, `ApplianceState`, or
anything protocol-level. A solid dot is a true, accurate claim: "the radio
link is fine." It was never a claim about whether the last thing sent
over that link was accepted.

### 1.6 Where a fix would slot in

Structurally, the smallest possible change is entirely local:
`HandleCommandOutcome()`'s existing `return false` branch for a rejected
dispense becomes a new `DispenseStatus`-adjacent signal into `DialState`,
`UiManager::Render()` gains one new branch reusing the existing
checkmark-toggle mechanism, and nothing else in the BLE/protocol stack
changes at all. Deliberately not scoped further than that here — see
§7, "Explicitly out of scope."

---

## 2. Concepts

### A — Temporary error overlay

A compact panel appears over the normal dial view for a few seconds, then
fades back.

This screen has never had a second visual layer — every state change today
is "the same widgets, different values," never "something drawn on top of
something else." An overlay needs new z-ordering, a new fade
choreography, and a new rule for what happens if the encoder is touched
while it's showing. That's real, new machinery for a screen whose entire
design history (both frozen specs) has been about removing mechanism, not
adding it.

**Rejected** — solves the problem, but at a cost this project has
consistently refused to pay elsewhere.

### B — Full dedicated error screen

The display swaps to a second, distinct layout — icon, message, its own
background treatment — then returns automatically.

The heaviest option. This UI has been exactly **one** screen since M2; a
second screen is a bigger structural change than the failure itself
warrants, and it's the closest of the four to feeling like "navigation,"
which the brief explicitly rules out even though this isn't a menu.

**Rejected** — disproportionate to the problem.

### C — Inline / status-based only

`hint_label_` gains a new message (e.g. `"Try again"`); nothing else about
the screen changes.

The cheapest possible fix, and worth taking seriously: it's a single-line
change to `Render()`'s existing hint ladder, no new state needed beyond
what `HandleCommandOutcome()` already discards. The weakness is legibility
at a glance — the brief explicitly asks for a signal that reads even in a
short look, and `hint_label_` is small, muted-grey utility text positioned
below the numeral. A user who glances at the dial mid-way through reading
a 6-character word gets no stronger a signal than "some text is there,"
which is close to today's actual failure mode (nothing draws attention at
all).

**Viable, but not the recommendation** — a legitimate fallback if D turns
out to cost more than expected to build.

### D — Transient fail glyph (recommended)

Reuses the exact mechanism already built for `Finished`: the numeral's
screen position gets a second hidden/shown label, `LV_SYMBOL_CLOSE`
instead of `LV_SYMBOL_OK`, shown for roughly 1.5 s (long enough to
actually read, unlike the 400 ms success hold which doesn't need to be
*read*, only *recognized*) — plus `hint_label_` reusing the exact same
slot Concept C would, and a one-shot ring desaturation to the same
`#6B8494` `Connection Lost` already uses, not sustained the way a real
disconnect is. Then automatic, unattended return to Ready.

Zero new LVGL objects, zero new layout, zero new navigation — it's Concept
C's minimal cost plus one already-proven idiom (the icon swap) doing the
"readable at a glance" job text alone can't. It also directly answers the
brief's "must not be confused with a BLE disconnect" requirement by
construction: a disconnect is *sustained* (grey ring + hollow dot +
"Connection lost" for as long as the link stays down); a command failure
is a single ~1.5 s pulse that lets go on its own, with the dot staying
solid throughout. Same colour vocabulary, structurally different
temporal signature — recognizably related, not identical.

**Recommended.**

---

## 3. Evaluation

| | New widgets | New navigation | Reads at a glance | Reuses existing idiom | Distinct from disconnect |
|---|---|---|---|---|---|
| A — Overlay | Yes (new layer) | No | Yes | No | Yes |
| B — Dedicated screen | Yes (new screen) | Effectively yes | Yes | No | Yes |
| C — Inline text only | No | No | Weak | Partial (text ladder) | Yes, but subtly |
| **D — Transient glyph** | **No** | **No** | **Yes** | **Yes (Finished's own mechanism)** | **Yes, structurally** |

---

## 4. Recommendation: Concept D

See the [artifact](https://claude.ai/code/artifact/de6b6724-2e25-436e-a983-471207ad7ca6)
for the full six-frame storyboard at real 240 px proportions. In prose:

1. **Ready** — 500 ml dialled, idle, ring in accent colour.
2. **Dispensing** — counting up, travelling highlight, ring fill
   unchanged from Ready (the one invariant this whole screen is built
   on — see the frozen spec §2.1).
3. **Finished** — checkmark in the numeral's slot, ~400 ms, unchanged
   from today.
4. **Command failed** *(new)* — `LV_SYMBOL_CLOSE` in the same slot, ring
   desaturated to `#6B8494`, `hint_label_` reads `"Try again"`, connection
   dot stays solid.
5. **Back to Ready** — automatic, ~1.5 s later, no button press required.

`"Try again"` (sentence case, matching the existing ladder's convention)
was chosen over a description of *what* failed: it's the single most
useful thing a generic, not-yet-classified failure can say, and it
matches this project's own copy instinct elsewhere — a control says what
happens, not how the system is built.

**Verification still needed, not done here:** `LV_SYMBOL_CLOSE` is a
standard LVGL built-in symbol from the same symbol font `LV_SYMBOL_OK`
already renders from — very likely already compiled in, since the
checkmark already proves that font is present, but this should be
confirmed against the actual build (`lv_conf.h`'s font/symbol
configuration) before implementation, not assumed.

---

## 5. Error classification

**Not proposing new BLE/protocol architecture here** — only describing
what already exists versus what a richer message would eventually need.

### Already available today, decoded and discarded

| Signal | Source | Carries |
|---|---|---|
| `CommandOutcome.state.is_success` | `grohe_ble::ApplianceState` | Accept/reject, already boolean |
| `CommandOutcome.state.response_code` | Same | The exact protocol code — `0` success, `1` `INVALID_HMAC`, `2` `TIMESTAMP_EXPIRED`, `3` `GUEST_MODE_DISABLED`, `4` `APPLIANCE_INTERNAL_ERROR` (`dial_state::ResponseCodeName()`) |
| `CommandOutcome.was_dispense` | Same | Whether this was a dispense or a stop, for correct wording |
| `CommandOutcome.available` | Same | An outcome exists at all (vs. none yet) |

A first, generic implementation (matching Concept D exactly as described)
needs **none** of the per-code detail above — `"Try again"` covers every
case in §4. The table exists to record that finer-grained messages are a
data-availability question already answered, not a future protocol change.

### Not yet available, would need new work later

- **A true timeout** (appliance never replies at all) — nothing today
  measures elapsed time against `command_pending_`; `CommandOutcome.
  available` staying `false` forever and "no outcome yet, still waiting"
  are currently indistinguishable.
- **BLE write failure itself** (`WriteCharacteristic()` returning non-OK,
  `grohe_client.cpp:106-109`) — already logged, already causes
  `RequestDispense()` to return `false` synchronously (so `HandleCommandSent
  (false)` already runs), but that path and a *rejected* command are two
  different failures a user-facing message would eventually want to tell
  apart.
- **BLE unreachable at the time of the press** — already fully handled
  today via `ConnectionStatus`/the existing hint ladder (a press can't
  even reach `kRequestDispense` in a way that matters if the link isn't
  `kReady`); not a new category, listed here only for completeness against
  the brief's own example list.

A future, cause-aware version of Concept D changes only `hint_label_`'s
text per case (`"Rejected"`, `"No reply"`, ...) — the icon, the ring
treatment, and the auto-return timing all stay identical. See the
artifact's "same treatment, different causes" strip for what that would
look like; it is deliberately *not* being built now.

---

## 6. BLE indicator: keep it connection-only

The brief's own instinct — don't mix connection status and command result
— holds up against the code, for three concrete reasons:

1. **It's already a narrowly, correctly scoped signal.** `ConnectionStatus`
   is computed from exactly `ble_ready_for_protocol_`/`ble_subscribed_`
   (§1.5) — link-layer facts, nothing else. During the actual hardware
   test that motivated this study, the link never dropped at all; the dot
   being solid the entire time was **accurate**, not a bug.
2. **`dial_state.hpp` already states this exact principle for a sibling
   pair.** `ConnectionStatus`/`TimeStatus`'s own comment: "Deliberately two
   separate fields, never combined into one... because they really are
   independent subsystems in the firmware underneath." Command result is a
   third, equally independent subsystem — conflating it into the
   connection dot would be the identical mistake that comment already
   warns against, one layer further down.
3. **Conflating them would destroy real diagnostic value.** "Connected,
   but the last command was rejected" (wrong credentials, guest mode,
   appliance-side error) and "not connected at all" are different
   *causes* requiring different *fixes* — a user re-provisioning
   credentials needs to know the BLE link itself is fine, or they'll waste
   time debugging the wrong layer.

**Recommendation: leave the connection dot exactly as it is.** Command
result gets its own, separate, transient signal (§4) — never borrows the
dot's colour or shape.

---

## 7. Explicitly out of scope (this study, and its eventual implementation)

- No BLE/Grohe protocol changes of any kind.
- No timeout measurement/new `CommandOutcome` fields (§5's "not yet
  available" list) — ships later, if at all, as its own, separate,
  reviewed change.
- No new LVGL widgets, screens, or navigation (ruled out explicitly by
  rejecting Concepts A/B).
- No change to the connection dot's own logic (§6).
- No MQTT/Home Assistant surfacing of dispense failures — a possible
  future M13.5 concern (optional live state, per `docs/ROADMAP.md`), not
  this study's.
