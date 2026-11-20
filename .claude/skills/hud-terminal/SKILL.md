---
name: hud-terminal
description: Interface with the BMW 6WB cluster and factory windscreen HUD over MOST FBlock 0x82 (HUDTerminal: instances 82/00 cluster and 82/02 HUD, image/text handles, function IDs, OpType D replies, palette indices, visibility sequence, and persistent terminal state) via native/src/hud (hud_proto, hud_sender, hud_most, hud_worker). Use when changing HUD rendering, decoding a worker --log, or diagnosing blank displays, timeouts, strays, or overlay artifacts.
---

# MOST HUDTerminal Protocol (`FBlock 0x82`)

Factory navigation draws turn graphics on the 6WB cluster and factory windscreen HUD through MOST function block **`0x82` (`HUDTerminal`)**. ExtendedHUD sends the same protocol calls from its standalone worker process.

**Reference test platform:** 2018 BMW F34 340i (B58, 8-speed automatic), NBT Evo (iDrive 6, firmware `NBTevo_X20511I`, split-node `hu-jacinto` / `hu-omap`), 6WB digital cluster + factory windscreen HUD.
Tags: **`[verified]`** observed on the reference vehicle; **`[static]`** from firmware inspection or protocol docs.

## Source Map

| Component | File | Role |
|---|---|---|
| Call encoding & validation | `native/src/hud/hud_proto.[ch]` | Encoders per FktID (`hud_polygon`, `hud_set_visibility`, …); `hud_call_valid` enforces terminal limits |
| Windowed / pipelined sender | `native/src/hud/hud_sender.[ch]` | Matches replies by `(fkt, handle)`; supports up to 32 in-flight calls and frame pipelining; 1,500 ms per-call timeout |
| MOST/DSI transport | `native/src/hud/hud_most.c`, `native/src/most/` | Connects to `DSYSMOSTServer` via `libsys_dsi_colibry*.so` (`dlopen` at runtime) |
| Scene & geometry | `hud_strip.c` (Sport Bar), `hud_cam.c` (speed cameras), `hud_scene.c` (`feature/nav` adds `hud_nav.c`, `hud_text.c`) | Pure C rendering logic, host-tested |
| Worker binary | `native/src/hud/hud_worker.c` → `hud-sportbar-qnx` | Probes, grids, palette swatches, parked/live rendering; `--log` prints every `tx`/`rx` |
| Log analyzer | `.claude/skills/hud-terminal/decode_log.py` | Summarizes calls sent, OpTypes received, unmatched replies, errors, and timeouts |

## Data Path

```text
hud-sportbar-qnx ──DSI──▶ DSYSMOSTServer (hu-jacinto) ──MOST──▶ 6WB  ─┬─ 82/00 cluster nav graphic
  (runs on hu-jacinto or hu-omap; hu-omap reaches server via Qnet)    └─ 82/02 factory HUD (host 0101)
```

- Calls use OpType **`6`** (`StartResultAck`) with the target handle as the first `uint16` payload field. Reference hardware replies with OpType **`D`** (`ResultAck`): accept `C` and `D` as success, `9`/`F` as errors, and `A` as processing status. A sender that accepts only `C` drops every reply `[verified]`.
- **Protocol & throughput limits:** ≤ 48 calls per batch, 1,500 ms per-call timeout, ~5 s batch deadline `[static]`. Individual replies take ~250 ms regardless of in-flight window (`3..32`). With merged geometry (≤ 12 calls/frame) and pipelined frames (`--pipeline`, window `32`), measured throughput is **~160 ms/frame (~6 fps)** vs. ~1.5–2 s unpipelined `[verified]`.
- The worker runs identically from `hu-jacinto` or `hu-omap` (`on -f hu-omap`, with `LD_LIBRARY_PATH=/lib:/usr/lib:/boot/lib:/opt/sys/lib`) `[verified]`.

## Handle Map (`82/02` Factory HUD)

| Handle | Dimensions on `82/02` | Purpose |
|---|---|---|
| **`31`/`32` (`0x1F`/`0x20`)** | 120×72 | **Double-buffered canvas**: active drawing pair to the right of the factory speed readout |
| `23` (`0x17`) | 250×116 | Navigation container: **`SetVisibility(0x17, 1)` is mandatory** before graphics appear; persists until sleep |
| `26`/`27` (`0x1A`/`0x1B`) | Text areas | `TextareaSetText` + `SetVisibility` renders factory-font street/distance text (Latin & Cyrillic) `[verified]` |
| `33`/`34`, `37`, `46` | 120×72 | Accepted, overlap `31`/`32`. **Always keep hidden** (see persistent state below) |
| `19`/`20`, `44`/`45` | — | Accepted, render no visible output |

Standard frame sequence: `ImageRemoveAllItems` on back buffer → set fill palette index → submit polygons (kind `2`/`3`, 1..99 vertices, concave polygons supported) → `SetVisibility(new, 1)` + `SwitchVisibility(old, new)`.
Shipped HUD flags: `--most-inst 2 --handles 31,32 --canvas 120x72 --container 23 --layout split --palette 3,4,3,4,3,4,4 --shift-colour 4 --backs fill --flash-ms 0 --pipeline`.

**Palette indices (`ImageSetFillColor 0x21B`, 1-byte index, not RGB):** `2`/`4`/`6` = white, `3` = dim grey, `5`/`8`/`11` = orange shades; `0`, `1`, `7`, `9`, `10`, `12+` draw nothing. No red, yellow, or blue exists. At night, the HUD automatically tints white toward orange. See `docs/HUD-DESIGN.md` for full Sport Bar design rules.

## Persistent Terminal State

`HUDTerminal` retains per-handle visibility and pen state until the **HUD/KOMBI enters sleep**. Restarting iDrive with the volume knob does **not** reset it `[verified]`.

| Persistent State | Trigger | Symptom | Cleanup |
|---|---|---|---|
| `SetVisibility(h, 1)` on `33`/`34`, `37`, `46` | Diagnostic grid tests | Soft/cropped overlay artifact on top of `31`/`32` | `--hide 33,34,37,46` (`SetVisibility 0`) |
| `SetVisibility(0x1A/0x1B, 1)` | Text-area tests (`--text-visible`) | Empty or stale text overlay boxes | `--text-hide` (worker also hides on exit) |
| Pen line width > 0 + line color | Line-swatch tests (`--swatch-line`) | Unexpected outlines on subsequent polygons | `--pen-reset` (line width `0` accepted) |
| `SetVisibility(0x17, 1)` | Any HUD draw | Required for display; benign | Leave enabled (hiding untested) |

**Rule:** Any diagnostic test that enables a handle or changes pen state must revert it before exiting. When visual output looks washed out or clipped after tests, audit residual visible handles before altering layouts or colors.

## Unmatched Error Replies

`ImageSetLineColor` (`0x218`) with palette index `0` returns **OpType `F` (`04 00 00`) with no handle bytes**, so the sender treats the reply as a stray and times out the original call after 1.5 s `[verified]`. Because stroked outlines are unreliable on hardware, draw outlines as thin filled polygons.

## Decoding Worker Logs

Never guess from high-level behavior when raw MOST logs are available:

```bash
ssh root@169.254.199.99 'on -f hu-omap sh -c "... ./hud-sportbar-qnx ... --log --seconds 25 2>&1"' > /tmp/hud-session.log
python3 .claude/skills/hud-terminal/decode_log.py /tmp/hud-session.log   # exits 1 on op F/9, strays, timeouts, or drops
```

Convert every newly observed protocol edge case or error sequence into a deterministic replay test in `native/tests/test_hud.c` using sanitized log lines.

## Development Workflow

- **Protocol changes:** Encode and validate new calls/handles in `hud_proto.c`, cover them in `native/tests/test_hud.c`, and test on hardware only via skill `car-run` (step-by-step confirmation, filming cluster and HUD).
- **Drawing changes:** Edit `hud_strip.c` / `hud_cam.c`, verify with `make -C native test-hud` (enforces batch size and canvas bounds), and inspect with `python3 native/tools/hud_preview.py -o /tmp/hud-preview.html`. Confirm visual changes with the user before deploying.
- **Navigation glyphs (`feature/nav`):** Regenerate `hud_glyphs.h` via `native/tools/hud_glyphs_gen.py` (Mapbox ManeuversStyleKit v0.40.0).
- **QNX build:** `sh native/docker/qnx.sh build` → `native/out/hud-sportbar-qnx`.
- **Single-sender rule:** Never run two HUD senders simultaneously (`HudTest`, Wayglass, NBT-AAD, or a manual test while ExtendedHUD is active). Uncheck **Show Sport Bar** and **Show camera warning** in iDrive before any manual worker invocation.
