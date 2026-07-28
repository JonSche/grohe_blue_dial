# Architecture

Grohe Dial is an ESP-IDF (C++20) firmware for the VIEWE
UEDX24240013-MD50E-B: an ESP32-C3 with a 240x240 round GC9A01 SPI display and
an onboard rotary encoder + button. See [`../README.md`](../README.md) for
build instructions and the pinout table.

## Goals behind the structure

- **One component, one concern.** Each component owns exactly one piece of
  hardware or one layer of the stack, and exposes a small C++ class instead
  of loose C functions and globals.
- **Dependencies point one way.** `board` sits at the bottom; `display`,
  `encoder` and `ui` each depend only on `board` (and, for `display`/`ui`,
  LVGL); `app` is the only component allowed to know about all of them at
  once. Nothing depends on `app`.
- **New hardware is a new sibling component**, not a change to existing
  ones. This is what lets a future BLE client slot in without touching
  `display`, `encoder`, or `ui` — see [Adding BLE](#adding-ble-grohebleclient)
  below.

## Component map

```
components/
  board/     Pin/bus constants only (header-only). No behavior.
  bringup/   ColorCycleTest: raw esp_lcd + esp_lcd_gc9a01 smoke test (no
             LVGL). Standalone diagnostic, not part of the app/ stack.
  display/   Gc9a01Display: SPI bus + esp_lcd_gc9a01 panel + esp_lvgl_port
             glue. Produces an lv_display_t* and a lock/unlock pair.
  encoder/   RotaryEncoder (GPIO-ISR quadrature decode) and Button
             (polled, debounced by the caller). No LVGL dependency.
  ui/        UiManager: the LVGL screen/widget tree. Takes an lv_display_t*
             and nothing else — no knowledge of SPI, GPIO, or esp_lcd.
  app/       App: the composition root. Owns one instance of each of the
             above, initializes them in dependency order, and runs the
             application loop.
main/        app_main() -> currently runs bringup::ColorCycleTest directly
             (see docs/ROADMAP.md, M0); switches to app::App from M2 on.
```

Dependency graph (arrows = "depends on"):

```
bringup --> board                (standalone; no lvgl, no esp_lvgl_port)

app --> display --> board
app --> ui       --> (lvgl only)
app --> encoder  --> board
```

`bringup` intentionally duplicates the handful of `esp_lcd`/`esp_lcd_gc9a01`
init calls that also appear in `display/gc9a01_display.cpp`, rather than
depending on `display` itself — pulling in `display` would pull in its
`lvgl`/`esp_lvgl_port` manifest dependencies too (the ESP-IDF component
manager resolves a component's declared dependencies as soon as that
component is part of the build, regardless of which of its classes are
actually used), which defeats the point of a bring-up test meant to isolate
panel/SPI/backlight issues from the UI framework. If this duplication grows
beyond basic panel bring-up, it's worth extracting a shared "raw panel init"
helper that both `bringup` and `display` depend on.

`ui` never includes `display/gc9a01_display.hpp`; it only takes the
`lv_display_t*` that `display` produces. This is why `ui` and `display` can
each change independently (e.g. swapping the panel driver, or replacing the
boot screen with a real dial UI) without touching one another.

## Runtime model

- **LVGL task**: owned by `esp_lvgl_port` (started in
  `Gc9a01Display::Init()`). It periodically calls `lv_timer_handler()` and
  drives the actual SPI flush. Nothing in this codebase talks to the panel
  directly outside of `Gc9a01Display::Init()`/`~Gc9a01Display()`.
- **Main task** (`app_main` / `app::App::Run()`): after bringing every
  subsystem up, this becomes a 50 ms poll loop
  (`app::App::PollInputs()`) that reads the encoder position and button
  state and logs on change. This is a placeholder for real UI-driving logic
  in later milestones (see [ROADMAP.md](ROADMAP.md)).
- **Locking**: LVGL itself is not thread-safe. Any code that touches LVGL
  objects from outside the LVGL task must hold the lock:

  ```cpp
  if (display::Gc9a01Display::Lock()) {
    // lv_* calls here
    display::Gc9a01Display::Unlock();
  }
  ```

  `app::App::Run()` already does this once, around `ui_.Init(...)`. Any
  future code that updates the UI from the main task (e.g. in response to a
  BLE event or an encoder turn) must follow the same pattern.

## Why GPIO-ISR (not PCNT) for the encoder

The rotary encoder is decoded in software via GPIO edge interrupts, not the
PCNT peripheral: **ESP32-C3 has no PCNT hardware at all** (`SOC_PCNT_SUPPORTED`
is unset for this target — confirmed absent from both
`soc/esp32c3/include/soc/soc_caps.h` and this project's generated
`sdkconfig`), so `esp_driver_pcnt` builds as an empty component here and any
`pcnt_*` call is an undefined reference at link time. This was originally
missed because `main.cpp` didn't link `app`/`encoder` into the binary until
well after the PCNT-based version was written — the very first real link
against this target caught it.

Both phase pins are configured `GPIO_INTR_ANYEDGE`; every edge is resolved
directly in the ISR via a standard 4×4 quadrature transition table indexed
by `(previous_state << 2) | new_state`, updating an atomic accumulator —
no queue, no dedicated decode task. `encoder::RotaryEncoder::GetPosition()`
is a relaxed atomic load.

## Adding BLE (`GroheBleClient`)

Not implemented yet, by design (see ROADMAP.md). When it lands, the
expected shape is:

1. A new `components/ble/` component (e.g. `GroheBleClient`) that wraps the
   NimBLE/Bluedroid GATT client and exposes a small C++ API of its own
   (connect, read/write characteristics, a status callback) — following the
   same pattern as `display` and `encoder`.
2. `app::App` gains one more member for it, constructs/initializes it
   alongside the others, and wires its callbacks to `ui::UiManager` (e.g.
   `SetStatusText()`) the same way `PollInputs()` currently logs encoder/
   button state.
3. `ui/` and `display/` should need **no changes** — `UiManager` already
   accepts arbitrary status text, and BLE has no reason to know about the
   panel or SPI bus.

If that turns out not to be true for some piece of the real BLE work, that's
a sign the current module boundary is wrong and worth revisiting rather
than working around.
