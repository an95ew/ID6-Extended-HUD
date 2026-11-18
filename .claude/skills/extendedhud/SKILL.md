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
               ▼                                 ▲ UDP 127.0.0.1:42873  struct drive_frame {sampled_ms, rpm, kph}
             shift-deck-qnx --host GW --udp-port 42873
               └─ OBD 01 0C (RPM) / 01 0D (speed) → DME 0x12, over HSFZ if the gateway answers, else the MOST tunnel
```

| Half | File | Holds |
|---|---|---|
| RHMI description | `native/plugin/ExtendedHUD.xml` | entry 1 → state 100; label 150 (model 50 Richtext status); checkboxes 151/152/153 → actions 21/22/23, models 51/52/53 |
| Manifest | `native/plugin/ExtendedHUD.manifest` | `<name>`/`<file>{pluginDir}/ExtendedHUD</file>`/DescFile; Etch connector 121, autoStart |
| Shell | `native/src/extended_hud.cpp` | handlers, `updateModel`, `setProperty`, spawn/reap of the worker, log |
| Logic (host-tested) | `native/src/hud/hud_app.[ch]` | defaults, `settings.txt` load/save, toggles, wants-worker, conflict check, **the frozen worker argv**, status text |
| Scene rules | `native/src/hud/hud_scene.c` | Sport Bar / Always / Navigation × driving mode → RPM, NAV or nothing (`menu-spec.md`) |

Always finish with:

```bash
python3 .claude/skills/extendedhud/check_app.py          # XML ↔ manifest ↔ extended_hud.cpp ↔ hud_app agree; connector/port unique
make -C native test                                       # host tests: hud_app, scenes, drawing, rg, MOST, diag_link
sh native/docker/qnx.sh package                           # out/libExtendedHUD.so, out/hud-sportbar-qnx, out/shift-deck-qnx, out/extended-hud.zip
sh .claude/skills/deploy-plugins/abi_check.sh out/libExtendedHUD.so
```

## Behaviour (what the owner sees)

- `settings.txt` in the app folder: `sport_bar always nav shift_rpm lang text street_max`. The installed defaults are **all three ticked**
  (`hud_app_defaults`), because the **driving mode is not wired** (`HUD_MODE_UNKNOWN`). Without Always the Sport Bar would never show,
  since unknown mode = Comfort. The worker re-reads the file every second.
- **Blank at idle is by design:** the strip lights from 1,000 rpm (`p->lo`), there are no unlit backs, and the gear digit is hidden at
  0 km/h. Idle (~840 rpm) in P shows nothing. Blip to see it. [car]
- "Always show Sport Bar" only works with Sport Bar ticked: `hud_app_toggle` ignores it, and the shell sets the checkbox's
  **property 1 (ENABLED, greys it)** and **2 (SELECTABLE)**. Property 2 alone made it unselectable but not grey (car). 1 is from BMW's
  RHMI numbering (IDriveConnectKit); confirm on the car. 3 = VISIBLE (BoostDeck uses it).
- The worker runs only while Sport Bar or Navigation is ticked. Unticking both stops it with SIGTERM, and it clears its picture.
- Refuses to start next to HudTest (`hud_app_conflict`). Two HUD senders freeze the HUD.
- Revs older than **2 s** count as stale (`HUD_DRIVE_STALE_MS`). Over MOST one RPM+speed frame takes ~550 ms, and the old 750 ms made the scene flap.
- **Navigation shows nothing live:** there is no CarPlay turn source yet (`route_active` never set). See
  `.claude/stories/hud-redesign/carplay-nav-source.md`, and ask the owner which option before working on it.

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
`want` table. **A new worker flag:** `hud_app_worker_argv` (`k_fixed`) + the argv assertions in `test_app()`. The look is frozen: ask the owner.

**Ship a build to the car:** follow deploy-plugins § "Update a file of an installed app" (new name, hash readback, `mv` over the old one,
then a knob restart). Never `cat >` over the loaded `.so` or over the running worker.

## Rules and pitfalls

- Never renumber ids or change connector 121 or the folder name: the menu cache (`RHMI-Cache`) and `settings.txt` live on.
- Never block the HMI thread: the shell only spawns, reaps and writes one small file. Drawing and diagnostics stay in the child processes.
- The worker's UDP port must not be ShiftDeck's `DRIVE_PORT` 42863: the installed ShiftDeck plugin binds it.
- `launch()` in `diagnostic_worker.h` sends the child's stderr to /dev/null. ExtendedHUD uses its own `spawnLogged`, so failures are visible.
- Name history: it was drafted as "HUD Sport Bar" / `HudSportBar`. The owner renamed it **ExtendedHUD** on 2026-11-18. The worker binary keeps
  its name `hud-sportbar-qnx`.
