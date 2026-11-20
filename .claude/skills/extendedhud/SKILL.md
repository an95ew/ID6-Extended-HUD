---
name: extendedhud
description: Change, build, debug or update ExtendedHUD, our installed iDrive app (My Vehicle → ExtendedHUD) that draws the M-style Sport Bar (and later CarPlay turns) on the factory HUD: plugin/ExtendedHUD.xml + manifest, src/extended_hud.cpp (libExtendedHUD.so), src/hud/hud_app.[ch] (settings, worker command line, status), and the worker/collector it starts. Use for menu rows, settings, the status line, why the HUD shows nothing, or shipping a new build to the car.
---

# ExtendedHUD

Installed on the owner's car on 2026-11-18 as `/var/opt/hmi/data/ID5/Plugins/ExtendedHUD/` (on hu-omap), connector **121**. It is
three processes, and none of them draws or does network I/O inside HmiMain3D:

```
HmiMain3D ── libExtendedHUD.so (menu shell, 1 s poll on the HMI thread)
               │ writes settings.txt  (tmp + rename)          ▲ ticks, status line
               │ spawn() + SIGTERM, restart back-off 2/5/15/60 s
               ▼
             hud-sportbar-qnx --draw-live …  ──DSI/MOST──▶ 82/02 HUD (handles 31/32, container 23, texts 26/27)
               │ posix_spawn (never fork: QNX refuses fork in a threaded process)
               ▼                                 ▲ UDP 127.0.0.1:42873  struct drive_frame (32 B: sampled_ms, rpm, kph,
             shift-deck-qnx --host GW --udp-port 42873 --gear --gear-every 2 --no-speed     gear_raw, mode_raw, source; -1 = not in it)
               └─ in turns: OBD 01 0C (RPM) → DME 0x12, UDS 22 DA 2E (gear) → EGS 0x18; HSFZ if the gateway answers, else MOST
   libExtendedHUD.so's CDS feed (cds.txt) sends drive_frame too, source CDS (wins while fresh)
```

| Half | File | Holds |
|---|---|---|
| RHMI description | `native/plugin/ExtendedHUD.xml` | entry 1 → state 100: status 150 (model 50); Sport Bar 151 / Always 152 (actions 21/22, models 51/52); Shift point 154 → state 101; Display Speed Camera Warning 155 / Orange camera warning 156 (actions 25/26, models 55/56); Warning distance 157 → state 102 (actions 41–43, label model 57); App information 158 → state 103 (labels models 58/59). Ids 23/53/153 = old Navigation: never reuse |
| Manifest | `native/plugin/ExtendedHUD.manifest` | `<name>`/`<file>{pluginDir}/ExtendedHUD</file>`/DescFile; Etch connector 121, autoStart |
| Shell | `native/src/extended_hud.cpp` | handlers, `updateModel`, `setProperty`, spawn/reap of the worker, log |
| Logic (host-tested) | `native/src/hud/hud_app.[ch]` | defaults, `settings.txt` load/save, toggles, wants-worker, conflict check, **the frozen worker argv**, status text |
| Scene rules | `native/src/hud/hud_scene.c` | Sport Bar / Always × driving mode → RPM or nothing; the camera warning on top (`menu-spec.md`) |

Always finish with:

```bash
python3 .claude/skills/extendedhud/check_app.py          # XML ↔ manifest ↔ extended_hud.cpp ↔ hud_app agree; connector/port unique
make -C native test                                       # host tests: hud_app, scenes, drawing, rg, MOST, diag_link
sh native/docker/qnx.sh package                           # out/libExtendedHUD.so, out/hud-sportbar-qnx, out/shift-deck-qnx, out/extended-hud.zip
sh .claude/skills/deploy-plugins/abi_check.sh out/libExtendedHUD.so
```

## Behaviour (what the owner sees)

- `settings.txt` in the app folder: `sport_bar always shift_rpm blink_rpm in_flight merge_pts cameras cam_dist cam_orange red`
  (old `nav lang text street_max` lines are ignored). The worker
  re-reads it every second.
- **Car data = CDS** (run 6, `cds.txt` shipped): `engine.RPMSpeed` (~100 ms), `driving.gear` (1 N, 2 R, 3 P, 5.. = 1st..), `driving.mode`
  (3 Comfort, 4 Sport, 5 Sport+, 7 Eco Pro); maps in `hud_gear.c`. CDS sends on change only: the plugin repeats the last values every
  poll. While CDS is fresh the collector's OBD values are ignored (`hud_live_feed`); the collector is the fallback.
- **The gear: D/S/M only, hidden in P/N/R, never estimated**; unmapped raw values show nothing and keep the bar off.
- **Speed:** a frame ≤ 12 calls (bar merged per colour, a polygon per digit: the HUD draws edges, joins across gaps showed as a
  line). `--pipeline` (in the app's argv) overlaps two frames; `in_flight` default 32. Car: ~160 ms a frame. The stats line has `lag`.
  Measure frame rate without `--log` (per-byte stderr over SSH slows the loop by a third).
- **Driving mode:** `hud_live_mode` → `hud_scene_select` (Sport/Sport+ without Always).
- "Always show Sport Bar" only works with Sport Bar ticked: `hud_app_toggle` ignores it, and the shell sets the checkbox's
  **property 1 (ENABLED, greys it)** and **2 (SELECTABLE)**. Property 2 alone made it unselectable but not grey (car). 1 is from BMW's
  RHMI numbering (IDriveConnectKit); confirm on the car. 3 = VISIBLE (BoostDeck uses it).
- The worker runs only while Sport Bar or Display Speed Camera Warning is ticked. Unticking both stops it with SIGTERM, and it
  clears its picture. Orange camera warning and Warning distance grey out while the warning is off (properties 1 and 2).
- **App information** (state 103): `HUD_APP_VERSION` / `HUD_APP_RELEASED` in `hud_app.h` (bump both per release) and the camera
  list's `# country` / `# date` from `cameras.txt`, read at every refresh, so a list updated by skill `update-speedcam-db` shows
  without a new build.
- Refuses to start next to HudTest (`hud_app_conflict`). Two HUD senders freeze the HUD.
- Revs older than **2 s** count as stale (`HUD_DRIVE_STALE_MS`). Over MOST one OBD RPM read takes ~260 ms; CDS is ~100 ms.
- **Navigation is not in this build:** the CarPlay turn scene lives on the `feature/nav` branch (with its skills
  `carplay-route-guidance` and `hud-glyphs`). Ask the owner before bringing any of it back.

## Debugging on the car (read-only first)

```bash
ssh root@169.254.199.99 'on -f hu-omap sockstat | grep 4287'          # worker alive = it holds UDP 42873 (pidin arg may not show it)
ssh root@169.254.199.99 'on -f hu-omap cat /tmp/hud-sportbar.log'      # worker + collector stdout/stderr, fresh per HMI start
ssh root@169.254.199.99 'on -f hu-omap cat /var/opt/hmi/data/ID5/Plugins/ExtendedHUD/settings.txt'
```

| Log shows | Means |
|---|---|
| `rpm 0 … batches 1` forever | no frames: collector not started (old fork bug), port taken (`Address already in use`: something else on 42873), or scene NONE (Always off + mode unknown) |
| `transport: MOST` then `rpm 8xx` | engine data flowing |
| `[hud] MOST link lost` / `82/02` missing | the DSI link; see hud-terminal |
| log empty, no socket | the worker exits at once: run its exact argv by hand **after unticking both items in the menu**, never next to the running one |

Collector trace: `SHIFTDECK_TRACE=1` in its environment prints the raw MOST exchange (`41 0C hh ll` = RPM×4).

## Recipes

**A new checkbox:** XML `raAction`, `raBoolModel`, `checkbox` (ids from `check_app.py`'s "free from"); a setting in `hud_settings` +
`hud_settings_parse/format`; `hud_app_toggle` + test; a constant, handler and `updateModel` in `extended_hud.cpp`; extend `check_app.py`'s
`want` table. **A new worker flag:** `hud_app_worker_argv` (`k_fixed`) + the argv assertions in `test_app()`. A knob to tune on the car: a
`settings.txt` key instead (`hud_settings_parse/format`, read by the worker every second). The look: ask the owner.

**Ship a build to the car:** follow deploy-plugins § "Update a file of an installed app" (new name, hash readback, `mv` over the old one,
then a knob restart). Never `cat >` over the loaded `.so` or over the running worker.

## Rules and pitfalls

- Never renumber ids or change connector 121 or the folder name: the menu cache (`RHMI-Cache`) and `settings.txt` live on.
- Never block the HMI thread: the shell only spawns, reaps and writes one small file. Drawing and diagnostics stay in the child processes.
- The worker's UDP port must not be ShiftDeck's `DRIVE_PORT` 42863: the installed ShiftDeck plugin binds it.
- `launch()` in `diagnostic_worker.h` sends the child's stderr to /dev/null. ExtendedHUD uses its own `spawnLogged`, so failures are visible.
- Name history: it was drafted as "HUD Sport Bar" / `HudSportBar`. The owner renamed it **ExtendedHUD** on 2026-11-18. The worker binary keeps
  its name `hud-sportbar-qnx`.

## Speed cameras and red (2026-11-19, branch feature/speed-cameras)

- `cameras.txt` in the app folder (built by `tools/cameras_build.py` from the Patrol Police table), passed as `--cameras`. The alert
  needs the car's GPS from CDS (`cds.txt` kinds `gps`, `heading`) and the engine running. Code: `src/hud/hud_cam.[ch]`.
- Settings keys: `cameras=0|1` (default 1), `red=N` (fill index of a red, 0 = none known: today's white look).
- Desk demo: `build/hud-worker --dump --scene cam ...`; car: `--draw-parked --scene cam` (hud_car_run.py K1/K1r).
