---
name: hud-glyphs
description: HUD maneuver pictures as convex polygon pieces from Mapbox icons, a parametric roundabout, a lane strip, and integer-only maths. Use when redesigning turn arrows or the roundabout, or when judging piece counts and call cost. For the drawing code here use hud-terminal (native/src/hud/hud_nav.c, native/tools/hud_glyphs_gen.py, native/src/hud/hud_glyphs.h).
---

# HUD maneuver pictures

Same HUDTerminal and `AddPolygon` call on a 6WA cluster and on this car. A generated glyph header, per-maneuver drawing, and a generator.

## Technique
- The terminal only fills **convex polygons** (type 3, 3..8 points), one MOST call per piece. A picture = list of pieces with a tone
  (0 route/bright, 1 context/dim). Piece count = call count = latency. About 20 ms per call on a 6WA; about 65 ms on this car (3 in flight).
- Picture box on 6WA is (70,20)-(170,120), y down. On an F54 only (35,15)-(85,65) showed, so those coordinates are remapped.
- **Glyphs:** Mapbox `ManeuversStyleKit.swift` / `LanesStyleKit.swift` from mapbox-navigation-ios **v0.40.0**. The generator
  interprets the PaintCode drawing code on a 32x32 canvas (transforms, bezier paths, fills, strokes), then:
  1. flattens curves (tolerance 0.08 units), simplifies outlines (Douglas-Peucker, 0.10);
  2. fills: ear-clip into triangles, merge back into convex pieces of <= 8 points (Hertel-Mehlhorn); a hole (destination pin) is
     drawn on top in the context tone;
  3. strokes: one overlapping quad per segment so joins close;
  4. scale to the box and round. Result: 12 glyphs, 180 pieces, plus the roundabout exit arrow (6 pieces).
- **Mirroring:** Mapbox draws only right-hand turns. Left turns are the same glyph with x -> 240 - x. Left-hand traffic mirrors whole
  pictures (roundabouts run clockwise). Digits are never mirrored.
- **Roundabout:** Mapbox's parametric `drawRoundabout`, computed for any exit angle: ring in the dim tone, the way round and the
  exit arrow in the bright tone, the whole composition shifted away from the arrow. Exits passed are thin roads (exits 2..4), or the
  junction's real roads when the phone sends a junction type plus angles. From exit 5 on the exit number is drawn in seven-segment
  digits. **Arcs are cut into pieces of <= 40 degrees so every piece stays convex.**
- **Lanes:** picture shrunk to 11/16 at the top of the box, lane-arrow strip under it (y 94..118). Lanes to take bright with the way
  out highlighted, the rest dim. Not mirrored for left-hand traffic.
- **No floating point, no variable division** in the car-side code (libc without soft-float): integer sine table (scale 1024),
  Mapbox canvas in 1/16 units.
- Check: build every maneuver type at many angles and sides, and confirm each piece is convex, has 3..8 points and lies inside the box.

## Pitfalls
- Every piece is a MOST call: watch the count per picture, not just the look.
- Concave shapes must be split; the roundabout ring is the main case.
- Generated header is never edited by hand or reformatted; regenerate and compare.
- Source download is pinned to a tag, not stored.

## In this repo
| Piece | Where |
|---|---|
| Mapbox glyph generation | `native/tools/hud_glyphs_gen.py` -> `native/src/hud/hud_glyphs.h` (pinned tag, sha256 checked) |
| Per-maneuver drawing, mirroring | `native/src/hud/hud_nav.c` |
| Parametric roundabout | Not drawn yet: the HUD still reads "arrow ?" (run 4 S2). Use the model above for the redesign. |
| Previewer | `native/tools/hud_preview.py` (desk) |
This car's box is 120x72 on 82/02 (handles 31/32). The 6WA box is 100x100.
