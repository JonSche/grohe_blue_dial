# Architecture

Grohe Dial is an ESP-IDF (C++20) firmware for the VIEWE
UEDX24240013-MD50E-B: an ESP32-C3 with a 240x240 round GC9A01 SPI display and
an onboard rotary encoder + button. See [`../README.md`](../README.md) for
build instructions and the pinout table.

## Goals behind the structure

- **One component, one concern.** Each component owns exactly one piece of
  hardware or one layer of the stack, and exposes a small C++ class instead
  of loose C functions and globals.
- **Dependencies point one way.** `board` and `dial_state` sit at the
  bottom (no dependencies of their own); `display`, `encoder` and `ui` each
  depend only on `board`/`dial_state` (and, for `display`/`ui`, LVGL); `app`
  is the only component allowed to know about all of them at once. Nothing
  depends on `app`.
- **New hardware is a new sibling component**, not a change to existing
  ones. This is what lets a future BLE client slot in without touching
  `display`, `encoder`, or `ui` — see [Adding BLE](#adding-ble-grohebleclient)
  below.

## Component map

```
components/
  board/       Pin/bus constants only (header-only). No behavior.
  dial_state/  DialState struct + WaterType enum + the amount range/step
               constants (header-only). The single source of truth for the
               dial UI; depends on nothing so both app/ and ui/ can share it
               without inverting the dependency graph.
  bringup/     ColorCycleTest: raw esp_lcd + esp_lcd_gc9a01 smoke test (no
               LVGL). Standalone diagnostic, not part of the app/ stack.
  display/     Gc9a01Display: SPI bus + a ported GC9A01 panel driver
               (lcd_panel_gc9a01.c, replacing the generic esp_lcd_gc9a01
               registry driver) + a self-owned LVGL v9 integration (no
               esp_lvgl_port). Produces an lv_display_t* and a lock/unlock
               pair.
  encoder/     RotaryEncoder (GPIO-ISR quadrature decode) and Button
               (polled, debounced by the caller) are the raw hardware
               access. EncoderInput wraps both and turns them into typed
               EncoderEvents (RotateCW/RotateCCW/ShortPress/LongPress) --
               no LVGL dependency, no knowledge of what the events mean.
  ui/          UiManager: the LVGL screen/widget tree for the dial screen
               (progress ring, amount, water type, hint). Takes an
               lv_display_t* and a DialState to render from -- no knowledge
               of SPI, GPIO, esp_lcd, or the encoder. Contains no business
               logic: Render() is a pure function of DialState.
  app/         App: the composition root, plus DialController -- the
               "Application Controller" that owns the one DialState
               instance and applies the interaction rules (amount
               clamping, water-type toggle, dispense logging) in response
               to EncoderEvents. App itself only wires
               EncoderInput::Poll() -> DialController::HandleEvent() ->
               UiManager::Render(); it contains no rules of its own.
main/          app_main() -> app::App.
```

Dependency graph (arrows = "depends on"):

```
bringup --> board                        (standalone; no lvgl)

app --> display --> board
app --> ui       --> dial_state, (lvgl)
app --> encoder  --> board
app --> dial_state
```

`bringup` intentionally duplicates the handful of `esp_lcd`/`esp_lcd_gc9a01`
init calls that also appear in `display/gc9a01_display.cpp`, rather than
depending on `display` itself — pulling in `display` would pull in its
`lvgl` manifest dependency too (the ESP-IDF component manager resolves a
component's declared dependencies as soon as that component is part of the
build, regardless of which of its classes are actually used), which defeats
the point of a bring-up test meant to isolate panel/SPI/backlight issues
from the UI framework. If this duplication grows beyond basic panel
bring-up, it's worth extracting a shared "raw panel init" helper that both
`bringup` and `display` depend on.

`ui` never includes `display/gc9a01_display.hpp`; it only takes the
`lv_display_t*` that `display` produces. This is why `ui` and `display` can
each change independently (e.g. swapping the panel driver, or replacing the
boot screen with a real dial UI) without touching one another.

## Runtime model

- **LVGL task**: self-owned, created and torn down entirely inside
  `Gc9a01Display::Init()`/`~Gc9a01Display()` — there is no `esp_lvgl_port`
  dependency. `Init()` sequences: `lv_init()` → allocate the frame buffer →
  `lv_display_create()`/`set_buffers()`/`set_flush_cb()` → register the
  panel IO's `on_color_trans_done` callback (calls `lv_display_flush_ready()`
  from ISR context) → start a 2ms `esp_timer` driving `lv_tick_inc()` →
  create a recursive mutex → create the dedicated LVGL task. That task loops
  `Lock() → lv_timer_handler() → Unlock() → vTaskDelay(10ms)` forever.
  Nothing in this codebase talks to the panel directly outside of
  `Gc9a01Display::Init()`/`~Gc9a01Display()`.
  - **Small row-based partial buffer, not a full-screen framebuffer.**
    (M3.2) A full 240×240 RGB565 frame is 115,200 bytes; a BLE memory RCA
    established that enabling NimBLE on ESP32-C3 shrinks the DMA-capable
    heap below that, and a follow-up architecture review established that
    neither LVGL, the GC9A01 (which has its own GRAM and full CASET/RASET
    windowed-write support — see `lcd_panel_gc9a01.c`), nor this UI's small,
    localized widget updates actually require a full-frame buffer — that
    was inherited from the vendor's reference `lvgl_port.c`, not a real
    requirement. `Gc9a01Display` now uses `LV_DISPLAY_RENDER_MODE_PARTIAL`
    with a 30-row buffer (14,400 bytes, ~87% smaller). LVGL computes buffer
    height as `buf_size / stride`, so this is exactly a "30 full-width rows"
    buffer, not an arbitrary byte count. Single-buffered, as before: LVGL
    waits for this (now much smaller and faster) buffer's flush to
    complete before rendering the next chunk, which remains imperceptible
    for this mostly-static round-dial UI.
  - **Task period is 10ms, not the vendor's 5ms.** `CONFIG_FREERTOS_HZ=100`
    means one tick is 10ms; `pdMS_TO_TICKS(5)` truncates to `0` ticks under
    integer division, which made `vTaskDelay()` a no-op `taskYIELD()`
    instead of a real sleep — the LVGL task then consumed ~99.5% of the CPU
    and starved the idle task, tripping `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0`'s
    watchdog every 5 seconds. 10ms is the smallest delay at this tick rate
    that actually blocks.
- **Main task** (`app_main` / `app::App::Run()`): after bringing every
  subsystem up, this becomes a 50 ms poll loop
  (`app::App::PollInputs()`) that reads the encoder position and button
  state and logs on change. This is a placeholder for real UI-driving logic
  in later milestones (see [ROADMAP.md](ROADMAP.md)).
- **Locking**: LVGL itself is not thread-safe. Any code that touches LVGL
  objects from outside the LVGL task must hold the lock (a recursive mutex,
  safe against nested `Lock()` calls from the same task):

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
