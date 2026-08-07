# Enclosure

A 3D-printable stand for the [VIEWE UEDX24240013-MD50E-B](https://viewedisplay.com/product/esp32-1-28-inch-240x240-round-tft-knob-display-gc9a01-arduino-lvgl/)
module — the same one shown in the [README](../../README.md)'s hero photo. The
module drops in as-is; nothing about it is modified. A cutout at the base
routes the USB-C cable out the back.

## Status

CAD source and print-ready files are being prepared for release here. If
you're reading this before that lands, the files aren't in the repository
yet — check back, or open an issue if you'd like to build one before the
files are up.

## What will ship here

- Print-ready `.stl` (or `.3mf`) files, sized for the exact module above —
  not a generic parametric mount.
- The original CAD source, once a format/tool is settled on.
- Print settings that are known to work (material, layer height, infill,
  supports).
- Assembly photos/steps — placing the module, routing the cable, any
  fasteners needed.

## Print & assembly (once files are published)

No soldering. No custom PCB. Assembly is: 3D-print the stand, drop the
module in, route the USB-C cable through the base cutout, done.

| Step | What |
|---|---|
| 1 | Print the stand (see print settings above once published) |
| 2 | Seat the display module into the recess — friction fit, no fasteners |
| 3 | Route the USB-C cable through the base cutout |
| 4 | Flash the firmware (see the main [README](../../README.md#quick-start)) |
