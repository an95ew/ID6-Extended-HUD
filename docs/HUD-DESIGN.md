# HUD design rules (ExtendedHUD on the F34 / 6WB factory HUD)

What a picture on the factory HUD can and cannot do, and the current Sport Bar design. Any new design or demo
follows this page. Tags: **[car]** seen on the car, **[static]** from the firmware or docs, **[inferred]**.

## The canvas

| Rule | Value | |
|---|---|---|
| Where we draw | 82/02 (factory HUD), image pair **31/32**, **120×72**, double buffer (`SwitchVisibility`) | [car] |
| What it needs | `SetVisibility(0x17, 1)` on 82/02 once; it stays set until the HUD sleeps | [car] |
| Position | right of the factory speed; the speed and the limit sign stay | [car] |
| Factory texts | 0x1A (street) and 0x1B (distance) show in the factory font **above** our image; 0x1B overlaps its top | [car] |
| Other images | 19/20, 44/45 accept draws but show nothing; 33/34, 37, 46 lie over 31/32 (the run-5 "shade"): keep them hidden | [car] |

## Colours

Fill colours are 1-byte palette indices, not RGB, set as pen state per image.

| Index | Looks | |
|---|---|---|
| **3** | dim grey; almost invisible by day, red-orange at night | [car] |
| **4** | white, like the factory speed; orange-tinted at night | [car] |
| 2, 6 | white | [car] |
| 5, 8, 11 | orange shades (rejected by the owner, run 5) | [car] |
| 0, 1, 7, 9, 10, 12–39 | accepted, draw nothing | [car] |

No red, yellow or blue exists. Separate zones by **shape**, not colour. Line colour 0 is refused, and outlines
failed in run 4, so draw everything as filled polygons.

## Speed

| Rule | Value | |
|---|---|---|
| Per call | ~65 ms with 3 in flight | [car] |
| Per frame | 20–24 calls take 1.3–1.85 s, so about **one frame every 1.5–2 s** | [car] |
| Batch | at most **48 calls**, 1.5 s per-call timeout, 5 s batch deadline | [static] |
| Blink | not usable: a 2 s blink works but the owner found it useless | [car] |

Design for it: every frame is RemoveAll, one pen per colour, one call per polygon, then the switch. Keep a frame
near 24 calls. Whole concave polygons fill cleanly (the nav arrows), so one shape is one call. [car]

## The Sport Bar

**Zones (owner, 2026-11-18):** six low fields 1000–5000, three taller 5000–6000, three tallest 6000–7000. The bar ends
at the B58 redline and soft limiter, 7000 (fuel cut 7200). The end is the menu's shift setting (default 7000); the
zones sit 2000 and 1000 below it. The RPM readout follows the engine past the end.

**Installed (run 4, approved):** twelve slanted fields across the top of the image, the higher zones taller. Lit fields
in 3 through the revs, the whole bar in 4 at the end (installed build: 6400, zones at 5200/5900), no unlit fields,
gear digit centred under the bar. Worst frame 20 calls.

**Proposed (2026-11-18, `--layout split`):**
- The whole bar is drawn in **3** (the track) and the lit fields in **4**, the same brightness as the factory speed,
  so the bar fills as the revs rise. At the shift point every field is lit.
- Under the bar: the **gear on the left** (left edge on the bar's) and the **RPM on the right** (right edge on the
  bar's, to the nearest 50, 4/5 of the gear's height), both in 4.
- Each digit is **one polygon** on a 3×5 grid; holes (0, 6, 8, 9) are keyholes on a horizontal bridge. The readout
  costs one call per digit. Worst frame 23 calls; only a changed key (fields, peak, gear, readout) redraws.
- Worker: `--layout split --palette 3,4,3,4,3,4,4 --shift-colour 4 --backs fill --flash-ms 0`.
- Car check before shipping: the keyhole holes show, and the dim track reads by day. [inferred until then]

Code: `native/src/hud/hud_strip.c` (`hud_geom_split`, `hud_digit_poly`), tests in `native/tests/test_hud.c`
(`test_split`). Desk previewer: `native/tools/hud_preview.py`.
