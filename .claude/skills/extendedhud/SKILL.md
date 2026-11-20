---
name: extendedhud
description: Modify, build, debug, or update the ExtendedHUD iDrive plugin (My Vehicle → ExtendedHUD) that renders the M-style Sport Bar and speed-camera warnings on the factory HUD: native/plugin/ExtendedHUD.xml + manifest, native/src/extended_hud.cpp (libExtendedHUD.so), native/src/hud/hud_app.[ch] (settings, worker argv, status), and the worker/collector processes. Use for menu changes, settings, status text, debugging a blank HUD, or preparing a build for deployment.
---

# ExtendedHUD Plugin Architecture and Maintenance

ExtendedHUD installs on the HMI node at `/var/opt/hmi/data/ID5/Plugins/ExtendedHUD/` with Etch connector **`121`**. It separates HMI interaction, HUD rendering, and OBD fallback into three processes so no drawing or network I/O runs on the `HmiMain3D` thread.

**Reference test platform:** 2018 BMW F34 340i (B58, 8-speed automatic), NBT Evo (iDrive 6, firmware `NBTevo_X20511I`, split-node `hu-jacinto` / `hu-omap`), 6WB digital cluster + factory windscreen HUD.

```text
HmiMain3D ── libExtendedHUD.so (menu shell, 1 s poll on HMI thread)
               │ writes settings.txt (tmp + rename)           ▲ ticks, status line
               │ spawn() + SIGTERM, restart back-off 2/5/15/60 s
               ▼
             hud-sportbar-qnx --draw-live …  ──DSI/MOST──▶ 82/02 HUD (handles 31/32, container 23, texts 26/27)
               │ posix_spawn (never fork: QNX refuses fork in threaded processes)
               ▼                                 ▲ UDP 127.0.0.1:42873  struct drive_frame (32 B: sampled_ms, rpm, kph,
             shift-deck-qnx --host GW --udp-port 42873 …                        gear_raw, mode_raw, source; -1 = absent)
               └─ fallback: OBD 01 0C (RPM) → DME 0x12 over HSFZ or MOST tunnel
   libExtendedHUD.so CDS feed (cds.txt) sends drive_frame too (source CDS takes priority while fresh)
```

## Component Map

| Layer | File | Responsibility |
|---|---|---|
| RHMI layout | `native/plugin/ExtendedHUD.xml` | Entry `1` → state `100`: status `150` (model `50`) + three submenu buttons (fixed-row pages don't scroll: 7 rows did not fit, car run 8). Sport Bar `186` (action `182`) → state `184`: Show Sport Bar `151` / Always `152` (actions `21`/`22`, models `51`/`52`), Shift point `154` → state `101`. Speed Camera Warning `187` (action `183`) → state `185`: Show camera warning `155` / Orange camera warning `156` (actions `25`/`26`, models `55`/`56`), Warning distance `157` → state `102` (actions `41–43`, label model `57`). App information `158` → state `103` (labels `180`/`181`, 900 px wide, models `58`/`59`; keep lines short, the page cuts ~40+ chars). IDs `23`/`53`/`153` are retired (former Navigation) — never reuse. |
| Manifest | `native/plugin/ExtendedHUD.manifest` | `<name>`, `<file>{pluginDir}/ExtendedHUD</file>`, `DescFile`; Etch connector `121`, `autoStart`. |
| Plugin shell | `native/src/extended_hud.cpp` | RHMI action handlers, `updateModel`, `setProperty`, CDS subscription, worker spawn/reap, logging. |
| App logic (host-tested) | `native/src/hud/hud_app.[ch]` | Defaults, `settings.txt` load/save, toggles, worker lifecycle check, conflict detection, frozen worker argv, status formatting. |
| Scene selection | `native/src/hud/hud_scene.c` | Maps Sport Bar / Always × driving mode and active speed-camera alerts to rendered HUD scenes. |

## Verification Pipeline

Always run after any code, XML, or manifest edit:

```bash
python3 .claude/skills/extendedhud/check_app.py          # Verify XML ↔ manifest ↔ extended_hud.cpp ↔ hud_app agreement & unique IDs
make -C native test                                      # Host unit tests (hud_app, scenes, drawing, cameras, MOST, diag_link)
sh native/docker/qnx.sh package                          # Build out/libExtendedHUD.so, workers, out/ExtendedHUD/, out/extended-hud.zip
sh .claude/skills/deploy-plugins/abi_check.sh out/libExtendedHUD.so
```

## Runtime Behavior

- **`settings.txt`:** Stored in the plugin directory with keys `sport_bar`, `always`, `shift_rpm`, `blink_rpm`, `in_flight`, `merge_pts`, `cameras`, `cam_dist`, `cam_orange`, and `red` (legacy `nav`, `lang`, `text`, `street_max` lines are ignored). Reloaded every second by the worker.
- **Primary vehicle data (CDS via `cds.txt`):** `engine.RPMSpeed` (~100 ms), `driving.gear` (`1` N, `2` R, `3` P, `5..` = 1st..), `driving.mode` (`3` Comfort, `4` Sport, `5` Sport+, `7` Eco Pro), plus GPS position/heading; mappings live in `hud_gear.c`. Because CDS emits on change only, the plugin repeats the last values on each 1 s poll. While CDS is fresh, OBD fallback values are ignored (`hud_live_feed`).
- **Gear policy:** Displayed in D/S/M only, hidden in P/N/R, never estimated from RPM/speed. Unmapped raw gear values keep the Sport Bar hidden.
- **Throughput:** Each frame uses ≤ 12 MOST calls (bar segments merged per color; one polygon per digit, since the HUD strokes polygon edges). `--pipeline` overlaps two frames with `in_flight=32` (~160 ms / ~6 fps on the reference head unit; `lag` is reported in stats). Measure frame rate without `--log`, as per-call `stderr` logging over SSH reduces throughput by ~30%.
- **Driving mode gating:** `hud_live_mode` → `hud_scene_select` restricts Sport Bar to Sport/Sport+ unless **Always show Sport Bar** is enabled.
- **Dependent menu items:** When Sport Bar is unchecked, `hud_app_toggle` ignores **Always show Sport Bar**, and the shell disables the checkbox via RHMI properties `1` (`ENABLED`, greys it out) and `2` (`SELECTABLE`). Similarly, **Orange camera warning** and **Warning distance** disable when camera warnings are unchecked. (Property `3` = `VISIBLE`.)
- **Worker lifecycle:** Runs only while **Sport Bar** or **Display Speed Camera Warning** is enabled. Unchecking both sends `SIGTERM`, and the worker clears its HUD buffer on exit.
- **App information (state `103`):** Reads `HUD_APP_VERSION` / `HUD_APP_RELEASED` from `hud_app.h` (bump both per release) and `# country` / `# date` from `cameras.txt` on every refresh, so camera database updates appear immediately without rebuilding.
- **Conflict guard:** Refuses to start if `HudTest` is installed (`hud_app_conflict`), preventing dual-sender HUD freezes.
- **Staleness threshold:** Drive frames older than **2 s** (`HUD_DRIVE_STALE_MS`) are ignored (MOST OBD takes ~260 ms/read; CDS ~100 ms).
- **Navigation branch:** CarPlay turn-by-turn navigation lives separately on `feature/nav` and is excluded from the main build; confirm with the user before reintroducing it.

## Debugging on a Target Vehicle (Read-Only First)

```bash
ssh root@169.254.199.99 'on -f hu-omap sockstat | grep 4287'          # Worker active = bound to UDP 42873 (pidin arg may omit it)
ssh root@169.254.199.99 'on -f hu-omap cat /tmp/hud-sportbar.log'     # Worker + collector stdout/stderr (fresh per HMI start)
ssh root@169.254.199.99 'on -f hu-omap cat /var/opt/hmi/data/ID5/Plugins/ExtendedHUD/settings.txt'
```

| Log Signature | Likely Cause |
|---|---|
| `rpm 0 … batches 1` indefinitely | No telemetry arriving: collector failed to start, UDP port `42873` occupied (`Address already in use`), or scene `NONE` (Always off + non-Sport mode) |
| `transport: MOST` followed by live `rpm` | Engine telemetry active over MOST fallback |
| `[hud] MOST link lost` / `82/02` missing | DSI/MOST HUDTerminal link issue — see skill `hud-terminal` |
| Empty log, no bound socket | Worker exited immediately; uncheck all ExtendedHUD options first, then run its exact argv manually to inspect output |

Enable `SHIFTDECK_TRACE=1` in the collector environment to inspect raw MOST exchanges (`41 0C hh ll` = RPM × 4).

## Common Modification Recipes

- **Add a checkbox:** Add `raAction`, `raBoolModel`, and `checkbox` to `ExtendedHUD.xml` (using IDs above `check_app.py`'s "free from" value); add the field to `hud_settings` and `hud_settings_parse`/`format`; update `hud_app_toggle` and unit tests; add constants, handler, and `updateModel` in `extended_hud.cpp`; update `want` in `check_app.py`.
- **Add a worker flag:** Update `k_fixed` in `hud_app_worker_argv` and corresponding argv assertions in `test_app()`. Prefer a `settings.txt` key (`hud_settings_parse`/`format`, reloaded every second) for runtime-tunable parameters. Visual layout changes require explicit user approval.
- **Deploy an update:** Follow `deploy-plugins` § "Update an Installed File (Atomic Swap)" (upload `.new`, verify SHA-256 readback, `mv` over target, then iDrive knob restart).

## Rules and Pitfalls

- **Stable IDs and paths:** Never renumber RHMI IDs, change connector `121`, or rename the `ExtendedHUD` directory — the HMI caches RHMI structures (`RHMI-Cache`) and preserves `settings.txt`.
- **Non-blocking HMI thread:** Keep all drawing, socket I/O, and diagnostics inside child processes; `libExtendedHUD.so` only spawns, reaps, and performs small atomic file writes.
- **Port hygiene:** Never reuse ShiftDeck's `DRIVE_PORT` (`42863`); keep ExtendedHUD on `42873`.
- **Visible child logs:** `diagnostic_worker.h`'s generic `launch()` redirects `stderr` to `/dev/null`; ExtendedHUD uses `spawnLogged` so worker and collector diagnostics remain visible in `/tmp/hud-sportbar.log`.
- **Legacy binary name:** The worker executable retains the filename `hud-sportbar-qnx` for backward compatibility.

## Speed-Camera Alerts and Palette Overrides

- `cameras.txt` in the plugin folder (generated per country by `tools/cameras_build.py` from `tools/cameras_sources.json`) is passed via `--cameras`. Alerts require vehicle GPS/heading from CDS (`cds.txt` kinds `gps`, `heading`) and a running engine. Implementation: `native/src/hud/hud_cam.[ch]`.
- `settings.txt` supports `cameras=0|1` (default `1`), `cam_dist=300|500|800`, `cam_orange=0|1`, and optional `red=N` (custom fill palette index; `0` keeps the default white palette).
- Desk simulation: `native/build/hud-worker --dump --scene cam ...`; parked vehicle preview: `--draw-parked --scene cam`.
