# Grohe Dial

Firmware for the [VIEWE UEDX24240013-MD50E-B](https://viewedisplay.com/product/esp32-1-28-inch-240x240-round-tft-knob-display-gc9a01-arduino-lvgl/):
an ESP32-C3 module with a 1.28" round 240x240 GC9A01 SPI display and an
onboard rotary encoder + button.

**Current milestone (M0):** raw hardware bring-up. `main` boots straight
into `bringup::ColorCycleTest`, which fills the whole screen with solid
red/green/blue/white/black, one second each, using only `esp_lcd` +
`esp_lcd_gc9a01` — no LVGL yet. This is deliberately the very first thing to
verify, before any UI framework is layered on top; see
[`docs/ROADMAP.md`](docs/ROADMAP.md).

The M1 milestone (LVGL boot screen showing "Grohe Dial") is already built
and lives under `components/{display,ui,app}`, just not currently wired to
`main` — see ROADMAP.md for when that switches back.

No BLE yet either way — the architecture is modular so a `GroheBleClient`
component can be added later without restructuring anything below.

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for how the components
fit together and [`docs/ROADMAP.md`](docs/ROADMAP.md) for what's next.

## Requirements

- ESP-IDF v5.3 or newer, with the IDF Component Manager (bundled by default).
- Target: `esp32c3`.

## Build

```sh
. $IDF_PATH/export.sh
idf.py set-target esp32c3
idf.py build flash monitor
```

For the current (M0) milestone, the only managed dependency is
`esp_lcd_gc9a01` — no network-fetched LVGL/`esp_lvgl_port` involved. Those
get pulled in once `main` is switched back to `app::App` (M1+).

## Layout

```
components/
  board/     Single source of truth for this board's GPIO pinout/bus config.
  bringup/   Raw esp_lcd + esp_lcd_gc9a01 color-cycle smoke test. No LVGL.
             This is what main/ currently runs (M0).
  display/   GC9A01 panel bring-up (SPI + esp_lcd) and the esp_lvgl_port glue
             that turns it into an lv_display_t.
  encoder/   Quadrature rotary encoder (hardware PCNT decode) + push button.
  ui/        LVGL screen/widget tree. Knows nothing about display or
             hardware drivers — just takes an lv_display_t*.
  app/       Composition root. Owns one instance of each of the above and
             wires them together; the only place that knows about all of
             them at once.
main/        Thin entry point: currently constructs bringup::ColorCycleTest
             and calls Run() (see M0 above); switches to app::App at M1+.
```

Each component only depends on the ones below it in that list (plus
`board`), so a new peripheral — most notably a future `GroheBleClient`
component for the BLE control channel — plugs in as a sibling component and
gets wired up in `app::App` without touching `display`, `encoder`, or `ui`.

## Board pinout (UEDX24240013-MD50E-B)

See [`components/board/include/board/board_config.hpp`](components/board/include/board/board_config.hpp)
for the authoritative list; summarized here:

| Function          | GPIO |
|--------------------|------|
| LCD SCLK           | 1    |
| LCD MOSI/SDA       | 0    |
| LCD CS             | 10   |
| LCD DC             | 4    |
| LCD TE (unused)    | 5    |
| LCD Backlight      | 8    |
| LCD Reset          | none (module has no reset line) |
| Encoder Phase A    | 7    |
| Encoder Phase B    | 6    |
| Button             | 9    |
| UART RX / TX       | 20 / 21 |
