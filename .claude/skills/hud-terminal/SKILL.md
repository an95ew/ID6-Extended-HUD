---
name: hud-terminal
description: Talk to the 6WB cluster/factory HUD picture over MOST (FBlock 0x82 HUDTerminal: instances 82/00 cluster and 82/02 HUD, image and text handles, function IDs, OpType D answers, palette indices, the visibility sequence, state that outlives our process) from native/src/hud (hud_proto, hud_sender, hud_most, hud_worker). Use for any change to what or how we draw on the HUD/cluster, reading a worker --log, or explaining a car result such as "nothing visible", timeouts, strays or a "shade" over the picture.
---

# The HUD terminal (FBlock 0x82)

The factory navigation draws its arrows on the 6WB cluster and the windscreen HUD through the **HUDTerminal** MOST function block
**0x82**. We send the same calls from our own process. Facts are tagged as in `.claude/rules/car-headunit.md`: **[car]** proven on the
owner's F34 340i (NBT Evo X20511I, 6WB, factory HUD), **[static]** from binaries or docs. The rules file holds the authoritative
tables; this skill shows how to use them.

| Part | File | Does |
|---|---|---|
| Call encoding + validators | `src/hud/hud_proto.[ch]` | one function per call (`hud_polygon`, `hud_set_visibility`, …); `hud_call_valid` = the packaged worker's limits |
| Batches, 3 in flight, answers | `src/hud/hud_sender.[ch]` | matches answers by (fkt, first u16 = handle); 1500 ms per-call timeout drops the rest of the frame |
| MOST/DSI link | `src/hud/hud_most.c`, `src/most/` | `DSYSMOSTServer` via `libsys_dsi_colibry…so`, `dlopen`ed at run time |
| What to draw | `hud_strip.c` (Sport Bar), `hud_cam.c` (speed-camera warning), `hud_scene.c`; the nav scene (`hud_nav.c`, `hud_text.c`, glyphs) is on the `feature/nav` branch | pure functions, host-tested |
| The process | `src/hud/hud_worker.c` → `hud-sportbar-qnx` | probes, grids, swatches, parked/live drawing; `--log` prints every tx/rx |
| Log reader | `.claude/skills/hud-terminal/decode_log.py` | per fkt: sent, answers by op, answers without a sent handle |

## The data path

```
hud-sportbar-qnx ──DSI──▶ DSYSMOSTServer (hu-jacinto) ──MOST──▶ 6WB  ─┬─ 82/00 cluster nav picture
  (jacinto or hu-omap; omap reaches the server over Qnet)             └─ 82/02 factory HUD  (host 0101)
```

- Calls are OpType **6** (StartResultAck) with the target handle as the first payload u16. **This car answers with op D**
  (ResultAck): accept C and D as success, 9/F as errors, A as a note only. A sender that waits for C drops everything (the run-1 bug). [car]
- Limits [static]: ≤ 48 calls per batch, ≤ 3 awaiting an answer, 1500 ms per answer, ~5 s per batch. In the car one call takes
  ~65 ms (3 in flight), so **20–26 calls ≈ 1.3–2.1 s per frame**: no fast blinks, few calls per frame. [car]
- The worker runs the same from hu-jacinto and from **hu-omap** (`on -f hu-omap`, `LD_LIBRARY_PATH=/lib:/usr/lib:/boot/lib:/opt/sys/lib`). [car]

## What draws where (82/02 = HUD)

| Handle | Size on 82/02 | Use |
|---|---|---|
| **31/32 (0x1F/0x20)** | 120×72 | **our double buffer**: the only pair that shows on the HUD, right of the factory speed |
| 23 (0x17) | 250×116 | nav container: **`SetVisibility(0x17,1)` is required** or nothing shows; it stays set |
| 26/27 (0x1A/0x1B) | text areas | `TextareaSetText` + `SetVisibility` shows street / distance in the factory font, Latin and Cyrillic [car] |
| 33/34, 37, 46 | 120×72 | accepted, draw in the same place as 31/32. **Never leave them visible** (see "State that outlives us") |
| 19/20, 44/45 | | accepted, nothing visible |

Working frame: `ImageRemoveAllItems` on the hidden buffer, fill colour, polygons (kind 2/3, 1..99 points, concave is fine), then
`SetVisibility(new,1)` + `SwitchVisibility(old,new)`. Worker flags for the HUD: `--most-inst 2 --handles 31,32 --canvas 120x72 --show both --container 23`.

**Colours are palette indices, not RGB** (fill `0x21B`): 2/4/6 white, 3 dim grey, 5/8/11 orange shades; 0, 1, 7, 9, 10, 12+ draw nothing. No red,
no yellow. At night the HUD tints white toward orange by itself (BMW's look: white by day, orange at night). **Design frozen 2026-11-18**:
strip `--palette 3,3,3,3,3,3,4 --shift-colour 4 --backs none --flash-ms 0 --layout big` (grey through the revs, all white at the shift),
nav dim 3 / ink 4. Don't change colours to "help" daylight: the owner raises the HUD brightness instead.

## State that outlives our process (the run-5 "shade")

The terminal keeps per-handle state until the **HUD/KOMBI sleeps**. An iDrive knob restart does **not** reset it. [car]

| State | Left by | Effect | Undo |
|---|---|---|---|
| `SetVisibility(h,1)` on 33/34, 37, 46 | run 4 grids T1a–c | lies over 31/32: soft, cropped right part ("shade circle") | `--hide 33,34,37,46` (SetVisibility 0) |
| `SetVisibility(0x1A/0x1B,1)` | nav with `--text-visible` | empty text boxes | `--text-hide`; the worker now hides them on exit |
| pen: line width 3 + a line colour | `--swatch-line` (T0) | outlines on every later polygon | `--pen-reset` (width 0 accepted) |
| `SetVisibility(0x17,1)` | every HUD draw | needed; harmless | never sent 0 (untested) |

**Rule: a test that makes something visible sets it back before it ends.** When a picture looks wrong after earlier tests, list
what earlier runs switched on before changing the design (run 5 changed colours twice for nothing).

## Answers that can't be matched

`ImageSetLineColor 0x218` with colour 0 is refused with **op F `04 00 00`: no handle**, so the sender sees a stray and times out after
1.5 s ("unanswered call", run 4 T0). Outlines are therefore unproven; draw outlines as thin polygons. [car]

## Read a car log, never guess

```bash
ssh root@169.254.199.99 'on -f hu-omap sh -c "... ./hud-sportbar-qnx ... --log --seconds 25 2>&1"' > /tmp/x.log
python3 .claude/skills/hud-terminal/decode_log.py /tmp/x.log      # exit 1 on op F/9, unmatched answers, timeouts, drops
```

Golden logs: `.claude/stories/hud-redesign/car-run-*/session-*/` (e.g. `W-pen-reset.log` has the 0x218 op F, `W-hide-33-34-37-46.log`
is clean). Each new failure mode becomes a replay test in `native/tests/test_hud.c` built from its real lines (example: the
`start_next` stale-`nflight` test from run 4 T0's "392 timeouts").

## Changing things

- New call or new handle: encode it in `hud_proto.c` with a validator, add it to `test_hud.c`, and try it on the car only through the `car-run`
  skill (one step, one yes, film cluster and HUD).
- Drawing: change `hud_strip.c` / `hud_cam.c`, then `make -C native test-hud` (it checks every frame fits a batch and every polygon is
  simple and on the canvas). Look at it with `python3 native/tools/hud_preview.py -o /tmp/p.html`. **Ask the owner before any visual change.**
- Glyphs (nav, `feature/nav` branch): `native/tools/hud_glyphs_gen.py` regenerates `hud_glyphs.h` from Mapbox's ManeuversStyleKit (v0.40.0).
- QNX build of the worker: `sh native/docker/qnx.sh build` → `native/out/hud-sportbar-qnx` (prints sha256).
- Never two HUD senders at once (HudTest, Wayglass, NBT-AAD, a manual test while ExtendedHUD runs): it freezes the HUD, and `slay` by name
  kills the installed app's copy too. Stop the app (untick Sport Bar and Display Speed Camera Warning) before a manual car test.
