# Grohe Dial — Dispense Animation Concepts

A design study for the one missing interaction: what the dial shows while
water is being poured.

Nothing here is implemented. This document exists to choose a direction
before any code is written.

---

## 0. Three constraints that decide everything

Before concepts, three facts from this project's own history. They eliminate
whole categories of idea, so they belong at the top rather than buried in a
feasibility footnote.

### 0.1 The target is an ESP32-**C3**, not an S3

The brief says ESP32-S3. The firmware targets `CONFIG_IDF_TARGET="esp32c3"`
(`sdkconfig.defaults:2`), and the board is the VIEWE UEDX24240013-MD50E-B.

This is not pedantry — it changes what is buildable:

| | ESP32-S3 (assumed) | ESP32-C3 (actual) |
|---|---|---|
| Cores | 2 × Xtensa @240 MHz | **1 × RISC-V @160 MHz** |
| PSRAM | commonly 2–8 MB | **none on this board** |
| SIMD / vector | yes (PIE) | no |
| Practical headroom | comfortable for effects | tight |

Single core matters most: the LVGL render task (`kLvglTaskPriority = 5`,
`gc9a01_display.cpp:30`) and the BLE host task share one CPU. An animation
that saturates rendering competes directly with the radio.

**If the hardware really is being changed to an S3, several rejected concepts
below (C5, C7) become viable and this study should be revisited.** As it
stands, everything is rated against the C3.

### 0.2 A full-screen canvas is impossible — this was already proven

- 240 × 240 RGB565 = **115,200 bytes**
- Current LVGL draw buffer = 30 rows = **14,400 bytes**
  (`gc9a01_display.cpp:147-150`)

The 115,200-byte allocation is *exactly* the full-screen framebuffer that
milestone M3.2 had to remove: enabling BLE made it unallocatable
(`docs/ARCHITECTURE.md:108-109`). Post-Wi-Fi heap at boot is ~53 KiB in the
main region.

**Any concept requiring an `lv_canvas` covering the disc is dead on arrival.**
Pixel-level water simulation, per-pixel refraction, and free-form particle
fields all fall here.

### 0.3 Dirty-rectangle economics dominate

At 80 MHz pixel clock a full-disc repaint costs **~11.5 ms of SPI alone**,
against a 10 ms LVGL task period (`kLvglTaskPeriodMs = 10`). A full-screen
animation cannot sustain smooth motion; it will beat against the task period
and jitter.

The practical rule:

| Region animated per frame | Verdict |
|---|---|
| A label's bounding box (~90 × 55 px) | free |
| A thin annular segment of the 210 px ring | cheap |
| A moving band across the disc | costly |
| The whole 240 × 240 disc | not sustainable |

**Motion should live on the ring and in the type — not across the face.**

---

## 1. The real design problem

The hardware constraints are the easy part. The genuinely hard problem is
this:

> **The firmware does not know how much water has actually been poured.**

`DispenseSession` runs a *predicted* duration from the flow model
(`1.32 s + amount_ml × 0.0403 s/ml`). The appliance sends one acknowledgement
and then nothing — no progress, no completion (established in
`docs/ARCHITECTURE.md`, "Dispensing").

So any animation that looks *precise* is making a promise the firmware cannot
keep. If the estimate runs fast, the ring completes while water is still
flowing. If it runs slow, the glass is full while the dial still claims 30 %
to go. Either way the product feels broken — and a premium product is one
that never visibly lies.

This reframes the brief. The question is not "which animation looks nicest"
but:

**Which animation communicates progress convincingly while remaining honest
about being an estimate?**

Three strategies follow from that, and each concept below commits to one:

- **(A) Embrace imprecision** — show activity and rhythm, not a percentage.
  Cannot be wrong, but tells the user little.
- **(B) Show progress, refuse to claim completion** — a determinate motion
  that decelerates asymptotically and never visually "lands" until the state
  machine actually says Idle.
- **(C) Show a different, honest quantity** — elapsed time or remaining
  volume, which are genuinely known (as estimates the user reads as
  estimates).

Strategy **B** is, I think, the mature answer, and it drives the final
recommendation.

---

## 2. What must not change

Per the brief, the layout stays recognisable. Current screen
(`components/ui/ui_manager.cpp`):

| Element | Position | Style |
|---|---|---|
| Progress ring (`lv_arc`) | centred, Ø210, 8 px | track `#2a3038`, indicator `#35a7e0` |
| Amount + "ml" | centre, y −16 | Montserrat 48 / 14 |
| Water type | y +34 | Montserrat 14, +2 letter-spacing |
| Hint | y +66 | Montserrat 14, 60 % opacity |
| Appliance status | y +88 | Montserrat 14, 60 % opacity |
| Background | — | `#101418` |

No element moves. No element is removed. The animation is expressed
*through* these, not alongside them.

One semantic note that shapes several concepts: **the ring already means
something.** At idle it shows the dialled amount against the 100–2000 ml
range. Whatever it does during a pour should feel like a continuation of that
meaning, not an unrelated second use of the same pixels.

---

## 3. Concepts

Eight concepts. Ratings are 1–10 and deliberately spread — a study where
everything scores 8 is useless.

---

### C1 — "Vessel": depleting ring + counting numeral

**Visual.** At the moment of SUCCESS the ring is full at the dialled amount.
It then drains anticlockwise back toward 12 o'clock as water is delivered.
The large numeral counts down in step, showing millilitres still to pour.
The ring keeps the accent colour; the track behind it stays visible, so the
"spent" portion reads as a hollowed-out arc.

**Timing.** Total = predicted duration. Motion is continuous, driven by one
`lv_anim_t`. The final 8 % uses a long ease-out so the last stretch visibly
*settles* rather than races to zero. The numeral updates at ≤4 Hz in steps of
10 ml — fast enough to feel live, slow enough not to flicker.

**Behaviour.** One meaning, two representations: both ring and numeral show
*what is left*. This is a continuation of the idle semantics ("your amount"),
not a mode switch — the ring still represents the request, now being consumed.

**Encoder.** Rotation is ignored during a pour (the amount is committed).
Optionally the ring gives a 2 px inward nudge to acknowledge input was seen
but not accepted — honest feedback beats silence.

**Stop.** Press → depletion decelerates over ~200 ms as if a valve were
closing, then settles at wherever it stopped. The numeral holds at the
residual value for ~600 ms so the user sees what was *not* poured, then the
screen returns to Idle showing the original dialled amount.

**Finished.** Ring reaches its floor, holds, then a single soft brighten →
fade of the indicator into the track colour (~320 ms), numeral crossfades
back to the dialled amount.

**Honesty.** Strategy B. The ring never fully empties and the numeral never
displays 0 — both hold at a small floor until the state machine leaves
Dispensing. Completion is signalled by the *settle*, not by a number.

**Implementation.** `lv_arc_set_value()` from one animation callback; label
text updated in the same callback, throttled. No new widgets.

**CPU/GPU.** Invalidation = the changed angular sliver + one label bbox. This
is close to the cheapest possible animation on this hardware.

**LVGL fit.** Native. `lv_anim_t` with a custom `exec_cb`, `lv_anim_set_path_ease_out`.

| | |
|---|---|
| Advantages | Semantically continuous with idle; readable at a glance and from across a kitchen; extremely cheap; handles estimate error gracefully; zero layout change |
| Disadvantages | Less "watery" than a literal fill; depletion direction needs a moment's learning on first use |
| Effort | **S** (~half a day) |
| Premium | **9** |
| Readability | **10** |
| Performance | **10** |

---

### C2 — Filling ring (conventional progress)

**Visual.** Ring starts empty and fills clockwise from 12 o'clock to full.
The numeral holds the dialled amount, static.

**Timing.** Linear or gentle ease-in-out across the predicted duration.

**Behaviour.** The familiar progress-bar mental model, bent round a circle.

**Encoder.** Ignored. **Stop.** Fill freezes, then fades. **Finished.** Ring
completes, brief hold, fade to idle.

**Honesty.** Weakest of the set — a ring that reaches 100 % is an explicit
claim of completion, and the estimate cannot support it.

**Implementation / CPU / LVGL.** Identical mechanics and cost to C1.

| | |
|---|---|
| Advantages | Instantly understood by everyone; trivial to build |
| Disadvantages | Generic — this is what every appliance, router and toothbrush does; conflicts with the ring's idle meaning (was "amount", now "percent"); makes a completion claim it can't honour |
| Effort | **S** |
| Premium | **6** |
| Readability | **9** |
| Performance | **10** |

---

### C3 — Orbiting comet

**Visual.** The ring stays at the dialled amount, dimmed to ~40 %. A short
luminous segment (~35°) with a soft trailing falloff orbits continuously.

**Timing.** One revolution per ~2.4 s, constant velocity, no easing — a
steady mechanical rhythm, like a turbine.

**Behaviour.** Indeterminate. Says "working" with confidence; says nothing
about how far along.

**Encoder.** Ignored. **Stop.** Comet decelerates to rest over ~300 ms.
**Finished.** Comet completes its current revolution then dissolves.

**Honesty.** Strategy A — cannot be wrong, because it claims nothing.

**Implementation.** A second `lv_arc` above the first with a fixed angular
span whose start angle animates 0→360 continuously.

**CPU/GPU.** Each frame invalidates the union of old and new segment
positions — a moderate annular band. Noticeably more than C1 but still
bounded to the ring.

**LVGL fit.** Good. The soft trailing falloff is the weak point: LVGL arcs
have no per-angle gradient, so the "comet tail" must be faked with 2–3
stacked arcs at decreasing opacity, which triples invalidation.

| | |
|---|---|
| Advantages | Elegant and calm; immune to estimate error; excellent as a *secondary* layer |
| Disadvantages | Conveys no progress at all — on a 2000 ml pour (~82 s) the user has no idea how long is left; the tail effect is awkward in LVGL |
| Effort | **S–M** |
| Premium | **8** |
| Readability | **5** |
| Performance | **8** |

---

### C4 — Breathing halo

**Visual.** Ring frozen at the dialled amount. The numeral and a soft
circular glow behind it breathe together — opacity 100 % ↔ 70 %, scale
1.00 ↔ 1.02.

**Timing.** ~3.2 s per cycle, sinusoidal. Deliberately slower than resting
human breath, which is what makes it read as calm rather than anxious.

**Behaviour.** Pure presence. The device is alive and busy.

**Encoder.** Ignored. **Stop.** Breathing settles to rest at full opacity
within one half-cycle. **Finished.** One final deeper inhale, then still.

**Honesty.** Strategy A.

**Implementation.** `lv_anim_t` on label opacity plus a background `lv_obj`
with animated `bg_opa`. Scale on text is best avoided (LVGL re-rasterises
glyphs — expensive and can shimmer); opacity alone achieves most of it.

**CPU/GPU.** The cheapest concept here — one label bbox.

| | |
|---|---|
| Advantages | Beautiful restraint; essentially free; impossible to get wrong |
| Disadvantages | Communicates almost nothing; on a long pour the user will wonder whether anything is happening; too passive to carry the interaction alone |
| Effort | **XS** |
| Premium | **8** |
| Readability | **3** |
| Performance | **10** |

---

### C5 — Meniscus fill

**Visual.** The disc fills with water from the bottom. A subtle surface line
with a slight meniscus curve rises; below it the background lifts to a
marginally lighter tone with the accent hue mixed in. The numeral floats
above, unaffected.

**Timing.** Surface rises across the predicted duration, with a very slight
lateral sway (±1.5 px, ~4 s period) so the surface reads as liquid rather
than a wipe.

**Behaviour.** The most literal and most emotionally direct: your glass is
filling.

**Encoder.** Ignored. **Stop.** Surface stops, sways to rest. **Finished.**
Surface holds, then drains downward quickly (~400 ms) — a satisfying release.

**Honesty.** Strategy B if it holds short of the brim.

**Implementation.** No canvas (§0.2). Approach: a child rectangle inside a
circular parent with `clip_corner`, animated in height; the meniscus is a
small pre-rendered sprite (240 × 12 RGB565 ≈ 5.6 KB — affordable) that rides
the surface.

**CPU/GPU.** The problem. A rising fill invalidates a full-width band every
frame, and the sway widens it. This is the "costly" row of §0.3. Expect
visible competition with the BLE task.

**LVGL fit.** Workable but fragile: `clip_corner` on a large filled child is
one of LVGL's more expensive paths, and there is no PSRAM to buffer around it.

| | |
|---|---|
| Advantages | The most beautiful and most on-brand idea here — it is literally what the product does; enormous emotional payoff |
| Disadvantages | The most expensive by a wide margin on a C3; real risk of jitter that would *destroy* the premium feel it is chasing; highest effort and highest risk of being abandoned late |
| Effort | **L** (2–3 days, plus tuning) |
| Premium | **9** |
| Readability | **8** |
| Performance | **4** |

---

### C6 — Concentric ripples

**Visual.** Soft rings expand outward from the centre and fade near the edge,
like drops falling into still water. Two or three alive at once.

**Timing.** A new ripple every ~1.6 s; each lives ~2.4 s, expanding with
ease-out while fading.

**Behaviour.** Rhythmic activity indicator.

**Encoder.** Ignored. **Stop.** Emission stops; in-flight ripples finish.
**Finished.** One larger final ripple, then still.

**Honesty.** Strategy A.

**Implementation.** Several `lv_obj` circles (radius `LV_RADIUS_CIRCLE`,
transparent fill, animated width + border opacity).

**CPU/GPU.** Each ripple invalidates its own annulus; three concurrent
ripples across most of the disc radius is a lot of scattered dirty area.

| | |
|---|---|
| Advantages | Pleasant; unmistakably water-associated |
| Disadvantages | Reads as *notification* or *scanning*, not *pouring*; risks feeling decorative — closer to a screensaver than an appliance state; no progress information; competes visually with the centred numeral |
| Effort | **M** |
| Premium | **6** |
| Readability | **3** |
| Performance | **6** |

---

### C7 — Flowing ring

**Visual.** The ring shows determinate progress *and* a continuous luminance
travelling along the filled portion — fluid moving through a pipe.

**Timing.** Progress across the predicted duration; the travelling highlight
loops every ~1.8 s independently.

**Behaviour.** The ideal marriage of "how far along" and "actively flowing".

**Honesty.** Strategy B.

**Implementation.** This is where LVGL fights back. Arcs have no gradient
along their sweep. The options are all poor: an image-based arc that must be
rotated (expensive without PSRAM), or a dozen stacked arc segments with
individually animated opacities (a dozen invalidations per frame).

**CPU/GPU.** Poor on both routes.

| | |
|---|---|
| Advantages | Conceptually the richest — conveys progress *and* liveness simultaneously |
| Disadvantages | Not practically achievable in LVGL on a C3 at acceptable cost; in practice it degrades into C3-over-C1, which is exactly the refinement already available cheaply (see §5) |
| Effort | **L** |
| Premium | **8** |
| Readability | **7** |
| Performance | **3** |

---

### C8 — Horizon sweep

**Visual.** A soft horizontal light band sweeps slowly down the face, once
per fixed volume increment (e.g. one sweep per 100 ml).

**Timing.** ~900 ms per sweep, spaced by the increment interval.

**Behaviour.** Progress expressed as *countable events* rather than a
continuous bar — you can literally count the pours.

**Encoder.** Ignored. **Stop.** Current sweep completes, then stops.
**Finished.** A final sweep that fades at the bottom.

**Honesty.** Strategy C, interestingly — it quantises the estimate, and
quantised information reads as inherently approximate.

**Implementation.** A gradient-filled child clipped to the circular parent,
animated in Y.

**CPU/GPU.** Same full-width band problem as C5.

| | |
|---|---|
| Advantages | Genuinely novel; the quantisation is an honest way to present an estimate |
| Disadvantages | Reads as "scanning" / photocopier; unrelated to water; expensive; the sweep repeatedly crosses the numeral, hurting legibility of the one thing that matters most |
| Effort | **M** |
| Premium | **5** |
| Readability | **5** |
| Performance | **5** |

---

## 4. Evaluation matrix

| # | Concept | Strategy | Effort | Premium | Readability | Performance | Mean |
|---|---|---|---|---|---|---|---|
| C1 | Vessel (depleting ring + counter) | B | S | 9 | 10 | 10 | **9.7** |
| C2 | Filling ring | — | S | 6 | 9 | 10 | 8.3 |
| C3 | Orbiting comet | A | S–M | 8 | 5 | 8 | 7.0 |
| C4 | Breathing halo | A | XS | 8 | 3 | 10 | 7.0 |
| C5 | Meniscus fill | B | L | 9 | 8 | 4 | 7.0 |
| C6 | Concentric ripples | A | M | 6 | 3 | 6 | 5.0 |
| C7 | Flowing ring | B | L | 8 | 7 | 3 | 6.0 |
| C8 | Horizon sweep | C | M | 5 | 5 | 5 | 5.0 |

Mean is a blunt instrument — it is shown for orientation, not as the verdict.

---

## 5. Top 3

**1. C1 — Vessel.** The only concept that scores well on all three axes
simultaneously, and the only one whose failure mode is graceful.

**2. C5 — Meniscus fill.** The most desirable design in the study and the one
I would most like to build. It is second only because the C3 cannot carry it
convincingly, and a stuttering water surface is worse than no water surface.
**Revisit immediately if the hardware becomes an S3 with PSRAM.**

**3. C3 — Orbiting comet.** Not as a standalone (readability 5 disqualifies
it) but as the strongest *additive layer* available at low cost.

---

## 6. Recommendation

> ## Build **C1 — "Vessel"**.

Optionally, as a phase-2 refinement *within* C1 rather than as a second
concept: a small luminous head at the leading edge of the depleting arc — a
2–3 px brighter cap that travels with the boundary. It borrows C3's liveness
for perhaps 30 extra lines and one extra small invalidation. It should be
built only after C1 is validated on hardware, and dropped without hesitation
if it costs frames.

### Why this one

**It fits the hardware.** Invalidation is a thin angular sliver plus one
label — the cheapest row in §0.3. It leaves the single C3 core free for the
BLE host task, which matters because this appliance's link is already
marginal at −95 to −101 dBm. An animation that starves the radio would
trade a cosmetic win for a functional regression.

**It fits the product vision.** Premium industrial design is subtractive.
Nothing is added to the screen; the elements already there simply acquire
motion and meaning. The result reads as a considered instrument rather than
a demo with effects layered on — which is exactly the gap between "premium
GROHE appliance" and "generic ESP32 project" the brief asks to close.

**It fits the existing UI.** Zero layout change, zero new widgets, and — the
part I care most about — zero semantic rupture. The ring means "your amount"
at idle and continues to mean "your amount" while pouring; it is simply being
spent. C2's filling ring, by contrast, silently redefines the same pixels
from *volume* to *percentage* mid-interaction.

**It fits the interaction model.** The dial is a two-state device: choose,
then commit. A countdown of what remains is the natural readout for the
committed state, and it answers the only question a user actually has while
standing at the tap — *how much longer?* C3, C4 and C6 cannot answer it at
all.

**It fits the truth.** This is decisive. Because the firmware only estimates,
the animation must degrade gracefully when the estimate is wrong. C1 does:
the asymptotic tail and the non-zero floor mean an early finish looks like
*settling* and a late one looks like *lingering* — both plausible physical
behaviours. A ring that hits 100 % (C2) or a water level that reaches the
brim (C5) turns the same estimation error into a visible lie.

### Honest cost

C1 is the least visually ambitious of the top three. It will not make anyone
gasp. What it will do is feel correct every single time, on the hardware that
actually exists — and for an appliance used several times a day, consistency
outranks spectacle.

---

## 7. Micro-interactions

All under 500 ms. Each is a single small invalidation.

| Event | Motion | Duration | Notes |
|---|---|---|---|
| **Button press** (encoder down) | Ring width 8 → 6 px, indicator brightens ~15 % | 120 ms ease-out | A tactile echo — the ring "gives" under the press. Reverses on release. |
| **Command accepted** (SUCCESS ack) | Single ring pulse 8 → 10 → 8 px; hint label crossfades "PRESS TO POUR" → "PRESS TO STOP" | 260 ms pulse, 180 ms crossfade | The one moment the appliance genuinely confirmed something — worth marking precisely once. |
| **Stop pressed** | Depletion decelerates as if a valve closed; indicator dims 100 % → 75 % | 200 ms ease-out | Deceleration, not a hard stop — hard stops feel like crashes. |
| **Finished** | Indicator brightens ~20 %, then fades into the track colour; numeral crossfades to the dialled amount | 320 ms total | Completion is announced by this settle, never by a number reaching zero. |
| **Error** | Numeral nudges ±3 px horizontally twice; accent desaturates toward muted for the duration | 90 ms per nudge, 400 ms desaturation | Deliberately **no red flash** — colour alarms read as cheap. Desaturation reads as "the system stepped back". |
| **Reconnect** | One faint 360° sweep around the *track* (not the indicator) at ~25 % opacity | 480 ms | Distinct from the comet: on the track, low opacity, exactly once. Signals re-establishment without demanding attention. |

Two rules across all six: nothing uses colour as the sole channel (opacity
and geometry carry the message too), and nothing moves the layout — only
weight, opacity and a few pixels of geometry.

---

## 8. Before committing

Two things should be measured, not assumed:

1. **Frame budget.** Instrument `lv_timer_handler()` duration during a C1
   animation with BLE connected. If the ring animation pushes the LVGL task
   past its 10 ms period, reduce the numeral update rate first (4 Hz → 2 Hz)
   before touching the ring.
2. **Legibility at distance.** Montserrat 48 counting down in 10 ml steps
   should be checked at ~1.5 m in a real kitchen. If the changing digits
   shimmer, switch the numeral to 25 ml steps rather than enlarging type.

And one open question for the product owner: **should the numeral count down
remaining or up toward the target?** This study assumes *down* (it pairs with
the depleting ring and answers "how much longer"). *Up* would pair with a
filling ring and answer "how much do I have". Both are defensible; they must
match each other, and the choice should be made deliberately rather than
inherited from whichever gets prototyped first.
