# HUD Design Specification (NBT Evo / 6WB Factory HUD)

Hardware constraints, palette rules, throughput characteristics, and Sport Bar visual design for ExtendedHUD. New HUD layouts or visual changes should conform to these specifications.

Evidence tags:
- **`[verified]`** — observed on the reference vehicle (2018 BMW F34 340i, B58, 8-speed automatic; NBT Evo iDrive 6, `NBTevo_X20511I`, split-node; 6WB cluster + factory windscreen HUD)
- **`[static]`** — derived from firmware inspection or protocol documentation
- **`[inferred]`** — deduced from related behavior, pending direct hardware confirmation

## Canvas

| Property | Specification | Tag |
|---|---|---|
| Target terminal | MOST `0x82/02` (factory HUD), image handle pair **`31`/`32`**, **`120×72`**, double-buffered via `SwitchVisibility` | `[verified]` |
| Container activation | `SetVisibility(0x17, 1)` on `82/02` once; persists until the HUD/KOMBI enters sleep | `[verified]` |
| Placement | Right of the factory speed readout; factory speed and speed-limit sign remain untouched | `[verified]` |
| Factory text areas | `0x1A` (street) and `0x1B` (distance) render in the factory font **above** the graphic buffer; `0x1B` overlaps its top edge | `[verified]` |
| Other image handles | `19`/`20` and `44`/`45` accept calls without visible output; `33`/`34`, `37`, and `46` overlap `31`/`32` (producing a washed-out/cropped overlay artifact) and must remain hidden | `[verified]` |

## Palette

Fill colors are 1-byte palette indices (not RGB), stored as per-image pen state.

| Index | Visual Appearance | Tag |
|---|---|---|
| **`3`** | Dim grey; subtle in bright daylight, tints reddish-orange at night | `[verified]` |
| **`4`** | White, matching the factory speed readout; tints orange at night | `[verified]` |
| `2`, `6` | White | `[verified]` |
| `5`, `8`, `11` | Orange shades (used optionally for speed-camera warnings; not used for Sport Bar) | `[verified]` |
| `0`, `1`, `7`, `9`, `10`, `12–39` | Accepted by terminal, render nothing | `[verified]` |

The 6WB HUD palette provides no red, yellow, or blue. Visual zones are therefore differentiated by **geometry** rather than color. `ImageSetLineColor` (`0x218`) with index `0` is rejected, and stroked outlines are unreliable on hardware, so all elements are rendered as filled polygons.

## Throughput and Protocol Limits

| Metric | Specification | Tag |
|---|---|---|
| Per-call latency | ~250 ms round-trip to reply, independent of in-flight window (`3..32`) | `[verified]` |
| Frame throughput | ≤ 12 calls per frame, pipelined (`--pipeline`), window `32`: **~160 ms (~6 fps)** vs. 1.5–2 s unpipelined | `[verified]` |
| Batch limits | Maximum **48 calls** per batch; 1.5 s per-call timeout; 5 s batch deadline | `[static]` |
| Polygons | Up to 99 points supported (51-point / 198 B polygons verified); the terminal strokes polygon edges too, so merge only contiguous shapes that touch | `[verified]` |
| Shift blink | Gear digit toggles once per rendered frame past the Shift point (~3 Hz at 6 fps) | `[verified]` |

**Design guideline:** Every frame performs `ImageRemoveAllItems`, sets at most one fill pen per color, merges contiguous bar segments per color, draws each digit as a single polygon, and finishes with `SetVisibility` + `SwitchVisibility`. Keep frames at or below ~12 calls to sustain ~6 fps. Concave polygons fill cleanly without tessellation `[verified]`.

## Sport Bar Layout (`--layout split`)

The shipped Sport Bar uses the split layout across the 120×72 canvas:

- **RPM zones (12 slanted segments):**
  - 6 low segments: `0–5,000` RPM (starting at 0 so idle illuminates part of the first segment)
  - 3 medium-height segments: `5,000–6,000` RPM
  - 3 tallest segments: `6,000–7,000` RPM
  - Default scale top is `7,000` RPM (matching the B58 soft limiter; fuel cut is `7,200` RPM). The zone boundaries track `shift_rpm - 2000` and `shift_rpm - 1000`. The numeric RPM readout continues tracking above the bar maximum.
- **Fill styling:** The complete 12-segment track renders in dim grey (`3`), while active segments up to the current RPM fill in white (`4`) matching the factory speed readout. At `shift_rpm`, all segments are lit; past `blink_rpm` (`6,200–6,600` RPM, default `6,400`), only the gear digit blinks.
- **Lower row:** Gear indicator aligned to the left edge of the bar (shown in D/S/M only; hidden in P/N/R) and numeric RPM aligned to the right edge (rounded to the nearest 50 RPM, 4/5 of gear height), both in white (`4`).
- **Single-polygon digits:** Each digit is encoded as a single polygon on a 3×5 grid; enclosed counters (`0`, `6`, `8`, `9`) use horizontal keyhole bridges so each digit requires only one MOST call. Redraws occur only when the visual state key (active segments, peak, gear, readout, or blink phase) changes.
- **Shipped worker arguments (`hud_app_worker_argv`):**
  ```text
  --draw-live --layout split --flash-ms 0 --palette 3,4,3,4,3,4,4 --shift-colour 4 --backs fill --most-inst 2 --handles 31,32 --canvas 120x72 --container 23 --udp-port 42873 --pipeline
  ```

Implementation: `native/src/hud/hud_strip.c` (`hud_geom_split`, `hud_digit_poly`), unit tests in `native/tests/test_hud.c` (`test_split`), and desk HTML previewer in `native/tools/hud_preview.py`.
