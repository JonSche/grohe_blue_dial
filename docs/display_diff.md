# Display bring-up diff: why the panel stayed black

Scope: `components/bringup/color_cycle_test.cpp` and `components/board/` only,
diffed against the manufacturer's working ESP-IDF reference for this exact
module (VIEWE `UEDX24240013-MD50E-B`):
`examples/ESP-IDF/UEDX24240013-MD50E-SDK-en/components/bsp/{bsp_lcd.c,lcd_panel_gc9a01.c}`
in `UEDX24240013-MD50ESP32_1.3inch-Knob`. Cross-checked against the vendor's
Arduino board header (`src/board/viewe/UEDX24240013-MD50E.h`), which encodes
the identical init table and pin config, confirming it's the verified
sequence for this panel and not a one-off.

## What matched (ruled out)

| Item | Ours | Manufacturer | Verdict |
|---|---|---|---|
| SPI pins (SCLK=1, MOSI=0, CS=10, DC=4) | ✓ | ✓ | match |
| Backlight pin/polarity (GPIO8, active-high, plain GPIO not PWM) | ✓ | ✓ | match |
| Reset line | none, `reset_gpio_num=-1` (SW reset) | `ESP_PANEL_LCD_IO_RST=-1` in the vendor's own board header | match at the time — see correction below |
| SPI mode, cmd/param bit widths | mode 0, 8/8 | mode 0, 8/8 | match |
| Pixel format / byte order (RGB565, byte-swapped for SPI) | ✓ | ✓ | match |
| `disp_on_off(true)` called after init | ✓ | ✓ | match |

## What differed (the actual bug)

Our code uses the community/Espressif-registry `esp_lcd_gc9a01` component
(`managed_components/espressif__esp_lcd_gc9a01`) with no `vendor_config`, so
it falls back to that component's built-in `vendor_specific_init_default`
table. That table is a **generic** GC9A01 bring-up sequence, not the one this
physical panel batch needs. Diffing it against the manufacturer's verified
table line-by-line:

- **Missing registers entirely**: `0xb5`/`0xb6` (frame-rate control),
  `0xbd`/`0xba`/`0xbc` (power control), `0x35`/`0x44` (tearing-effect config),
  and **`0x21` (Display Inversion ON)**.
- **Different values for shared registers**: `0x89` (`0x23` vs `0x21`),
  `0xc9` (`0x30` vs `0x25`), `0xe8` (`0x34` vs `0x04`).
- **Different gate/VGH-VGL bias in the gamma tables** `0x62`/`0x63`: our
  table's first/seventh bytes are `0x38`, the manufacturer's are `0x18`.
  These plus an extra, manufacturer-absent `0x60`/`0x61` pair are internal
  gate-driver voltage settings, not cosmetic gamma curve tweaks.

None of this is caught by `ESP_ERROR_CHECK`: `esp_lcd_panel_io_tx_param`/
`tx_color` just clock bytes out over SPI and return `ESP_OK` — the GC9A01
never reports back whether the values it received put the panel into a
displayable state. A wrong power/bias register still "succeeds" as an SPI
transaction while leaving the panel's internal drive voltages unable to
produce visible contrast, and a missing Display-Inversion-On leaves this
particular (inverting-type) panel constantly showing its clipped/undriven
state — both read as solid black regardless of what's in the frame buffer.
This matches the exact symptom: `esp_lcd_panel_draw_bitmap()` returns `ESP_OK`
every time, the log cycles through all five fills forever, and the screen
never lights up.

## Root cause (ranked)

1. **(Primary, ~90% confidence)** Wrong vendor init sequence: the generic
   `vendor_specific_init_default` in the registry `esp_lcd_gc9a01` component
   never sends the power-control registers (`0xb5/0xb6/0xbd/0xba/0xbc`) and
   uses different VGH/VGL bias bytes than this panel needs, so the panel's
   internal voltage generation doesn't come up correctly — SPI traffic
   succeeds, pixel data is written, but the LC drive voltage never reaches a
   state that shows contrast.
2. **(Secondary, ~9%)** Missing `0x21` (Display Inversion ON): this GC9A01
   variant needs inversion enabled; without it the panel may clip to a single
   extreme (reads as solid black or solid white, not the expected color).
3. **(Cosmetic, ~1%)** `MADCTL` is `0x00` (RGB, no mirror) instead of the
   panel's native `0x48` (MX + BGR) — would show mirrored/wrong-colored
   frames, not a black screen, so this alone doesn't explain the symptom.

Rank 1 and 2 are both fixed by the same change, since both missing pieces
live in the same custom init table.

## Fix implemented

The manufacturer's verified command table (copied byte-for-byte from
`lcd_panel_gc9a01.c`) lives in its own standalone component,
`components/gc9a01_vendor/gc9a01_vendor_init.{hpp,cpp}`, exposing a single
`gc9a01_vendor::kGc9a01VendorConfig` (a `gc9a01_vendor_config_t` — the
`esp_lcd_gc9a01` component's public extension point for exactly this
situation, see `esp_lcd_gc9a01.h`). This component's `CMakeLists.txt`
`REQUIRES` only `esp_lcd_gc9a01` — no LVGL, no dependency on `display`.
Both `components/bringup/color_cycle_test.cpp` and
`components/display/gc9a01_display.cpp` list `gc9a01_vendor` in their own
`REQUIRES` and set `panel_dev_config.vendor_config` to
`&gc9a01_vendor::kGc9a01VendorConfig` before calling
`esp_lcd_new_panel_gc9a01`, so the raw bring-up path and the LVGL display
path program the panel identically, and the bring-up test's build graph
stays exactly as LVGL-free as before this table was shared.

## Why this fixes it (evidence, no speculation)

- The manufacturer's `bsp_lcd.c` example is confirmed working on this exact
  hardware by the user.
- `esp_lcd_gc9a01.h` documents `vendor_config`/`gc9a01_vendor_config_t` as
  the supported mechanism for supplying a panel-specific init table in place
  of the built-in generic one — this isn't a workaround, it's the driver's
  intended extension point.
- `panel_gc9a01_init()` in the registry driver sends `SLPOUT` and the
  computed `MADCTL`/`COLMOD` first, then iterates whatever table is in
  `vendor_config` (falling back to the generic one only if `vendor_config`
  is `NULL`). Supplying the manufacturer's table means every register the
  manufacturer's own working example writes now gets written here too, in
  the same order, with the same values — including the power-control and
  inversion registers that were previously never sent at all.

## Correction: the reset line row above was wrong

The "Reset line: match" row above was cross-checked only against the
manufacturer's **Arduino/ESP_Panel** board header
(`src/board/viewe/UEDX24240013-MD50E.h`, `ESP_PANEL_LCD_IO_RST=-1`). A later
task ported the manufacturer's **ESP-IDF** reference directly
(`components/bsp/bsp_lcd.c`), which unambiguously defines
`PIN_NUM_LCD_RST = GPIO_NUM_2` and drives a real hardware reset pulse before
init. The manufacturer's two SDKs for the same physical board disagree on
whether a reset line is used at all — this wasn't caught earlier because
that task only compared against the Arduino header, not the ESP-IDF one.

Since the ESP-IDF reference is the one being ported from this point on,
`board::kLcdPinReset` now uses `GPIO_NUM_2` (see `board_config.hpp`), and
`esp_lcd_panel_reset()` performs a real hardware reset rather than the
`SWRESET` software fallback. This doesn't retroactively invalidate the root
cause above (the vendor init table was still the reason for the black
screen; reset polarity was never in question), it just corrects which pin
config is actually "the verified one" for the ESP-IDF port.
