# ExtendedHUD

ExtendedHUD renders an M-style sequential rev bar (**Sport Bar**) and **speed-camera warnings** on the factory windscreen HUD of BMW NBT Evo (iDrive 6) vehicles equipped with a 6WB instrument cluster. It appears under **My Vehicle → ExtendedHUD**.

- **Release:** 0.0.1 (2026-11-20)
- **Installation guide:** [../INSTALL.md](../INSTALL.md)
- **Navigation note:** CarPlay turn-by-turn HUD navigation is under development on the `feature/nav` branch and is not included in this build.

## Reference Test Platform

All hardware-verified behavior in this documentation was tested on:

| Component | Specification |
|---|---|
| Vehicle | 2018 BMW F34 340i (B58, 8-speed automatic) |
| Head unit | NBT Evo (iDrive 6), firmware `NBTevo_X20511I`, split-node (`hu-jacinto` SSH / `hu-omap` HMI) |
| Cluster / HUD | 6WB digital instrument cluster with factory windscreen HUD |

Other NBT Evo firmware versions and F-series 6WB/HUD configurations may be compatible, subject to framework ABI verification prior to installation.

## Architecture

```text
HmiMain3D ── libExtendedHUD.so        HMI menu, settings persistence, worker lifecycle (no drawing or network I/O on the HMI thread)
               │ spawn + SIGTERM
               ▼
             hud-sportbar-qnx         Draws to factory HUD over MOST FBlock 0x82 (HUDTerminal), instance 2
               │ posix_spawn                     ▲ UDP 127.0.0.1:42873, {sampled_ms, rpm, kph, ...}
               ▼
             shift-deck-qnx           Read-only OBD 01 0C (RPM) and 01 0D (speed) to DME 0x12 over HSFZ or the MOST tunnel
```

- The plugin persists configuration to `settings.txt` in its installation directory (`sport_bar`, `always`, `shift_rpm`, `blink_rpm`, `cameras`, `cam_dist`, `cam_orange`, etc.) using atomic write-and-rename. The worker reloads the file every second.
- The worker runs only while **Show Sport Bar** or **Show camera warning** is enabled, and clears its HUD canvas when stopped.
- The OBD collector (`native/src/collector.c` → `shift-deck-qnx`) is strictly read-only and issues only OBD service `01` requests. Its UDP port (`42873`) is distinct from `DRIVE_PORT` (`42863`) so it can coexist with ShiftDeck if installed.
- Primary vehicle telemetry comes from the HMI Car Data Service (CDS, below): engine RPM approximately every 100 ms, with gear and driving mode reported on change. The OBD collector (~260 ms per request over the MOST tunnel) acts as an automatic fallback when CDS is idle. Frames older than 2 s are treated as stale and not rendered.

## HUD Terminal (`0x82/02`)

The 6WB cluster renders graphics sent to MOST function block **0x82** (`HUDTerminal`): instance **0** drives the cluster navigation display, and instance **2** drives the windscreen HUD. Calls use OpType `6` (`StartResultAck`) with the target handle as the leading `uint16`; the reference unit responds with OpType **`D`** (`ResultAck`).

| Handle (`82/02`) | Size | Role |
|---|---|---|
| `31`/`32` (`0x1F`/`0x20`) | 120×72 | Double-buffered drawing canvas to the right of the factory speed readout |
| `23` (`0x17`) | 250×116 | Navigation container (`SetVisibility(0x17, 1)` is required before graphics appear) |
| `26`/`27` (`0x1A`/`0x1B`) | Text areas | Factory-font street and distance fields, Latin and Cyrillic (used on `feature/nav` only) |

Each frame sends `ImageRemoveAllItems` on the back buffer, sets the fill palette index, submits polygons, and finishes with `SetVisibility` + `SwitchVisibility`. Colors are 1-byte palette indices: `2`, `4`, and `6` render white; `3` renders dim grey; `5`, `8`, and `11` render orange shades. At night, the factory HUD automatically tints white toward orange. Visibility and pen state persist inside the terminal until the HUD/KOMBI sleeps, so any test that enables another image handle must hide it again before exiting. Because the HUD strokes polygon edges as well as filling interiors, polygons are only merged where shapes physically touch.

The **Sport Bar** uses the split layout: the full track remains visible in dim grey (index `3`), active segments fill in white (`4`) matching the factory speed readout, the current gear sits on the lower left, and numeric RPM sits on the lower right. It displays only when the engine is running and the transmission is in D, S, or M (including idle). With the engine off or in P, N, or R, the canvas stays clear. Gear data comes strictly from CDS `driving.gear` and is never estimated from RPM and speed; unmapped raw values are treated as non-drive states. Above the configured **Shift point** (`6,200–6,600` RPM, default `6,400`), only the gear digit blinks (one toggle per rendered frame).

**Frame rate.** Each frame takes at most 12 MOST calls (bar segments merged per color; one polygon per digit). Because individual replies take ~250 ms regardless of the in-flight window on the reference head unit, the worker pipelines frames (`--pipeline`): the next frame begins into the alternate buffer as soon as the current frame's calls are transmitted. With `in_flight=32`, measured throughput on the reference hardware is **~6 fps (~160 ms per frame)**, compared to ~1.8 s unpipelined (`window 3 → 828 ms`, `8 → 378 ms`, `16 → 285 ms`, `pipelined 32 → 161 ms`).

## iDrive Menu

The main page holds the status line and three submenus: **Sport Bar**, **Speed Camera Warning** and **App information**
(a page with fixed rows does not scroll, so the options live in the submenus).

| Submenu → row | Behavior |
|---|---|
| Status line (main page) | `Off` / `Starting` / `Running` (active features summary) |
| Sport Bar → Show Sport Bar | Enables or disables the sequential rev bar |
| Sport Bar → Always show Sport Bar | Shows the bar in all driving modes; disabled while Sport Bar is off |
| Sport Bar → Shift point | Submenu with five options (`6,200–6,600` RPM) controlling gear-blink threshold |
| Speed Camera Warning → Show camera warning | Enables or disables GPS-based fixed speed-camera alerts |
| Speed Camera Warning → Orange camera warning | Renders camera icon and speed limit in orange (palette `5`) instead of white; disabled while camera warning is off |
| Speed Camera Warning → Warning distance | Submenu: `300`, `500` (default), or `800` m lead distance; disabled while camera warning is off |
| App information | Displays app version, release date, and installed camera database as `Speed cameras: <ISO>-<count>-<YYYY-MM-DD>` (e.g., `UA-426-2026-11-19`) |

When **Always show Sport Bar** is unchecked, the bar appears only in Sport and Sport+ (read from CDS `driving.mode`).

Legacy keys in `settings.txt` (`nav`, `lang`, `text`, `street_max`) are ignored. Optional tuning keys with no menu row can also be set in `settings.txt` (reloaded every second):
- `red=N`: custom fill palette index for shift-zone highlights and camera alerts (`0`, default, keeps white because the 6WB HUD palette has no red).
- `in_flight=1..32`: maximum concurrent HUD calls awaiting replies (default `32`).
- `merge_pts=-1..99`: vertex limit per merged polygon (`0` disables merging; `-1` uses the layout default).

### Car Data Service (CDS)

The shipped `cds.txt` file in the plugin directory lists one `<property> <kind> [interval]` entry per line (`log`, `rpm`, `kph`, `gear`, `mode`, `gps`, `heading`). The plugin logs values to `/tmp/hud-cds.log` and forwards non-`log` signals to the worker. Because CDS emits updates only on change, the plugin repeats the last known values once per second.

During CDS registration, the plugin writes a `cds.guard` file and removes it 30 s later. If initialization crashes inside CDS registration, `cds.guard` remains on disk and subsequent boots skip CDS until the file is deleted.

| Property (`NBTevo_X20511I`) | Observed Format / Mapping |
|---|---|
| `engine.RPMSpeed` | `{"RPMSpeed":660}`; ~100 ms updates at interval `50` |
| `driving.gear` | `1` = N, `2` = R, `3` = P, `5` = 1st, `6` = 2nd (verified on reference vehicle); `7..12` = 3rd..8th (inferred from sequential counter) |
| `driving.mode` | `3` = Comfort, `4` = Sport, `5` = Sport+, `7` = Eco Pro |
| `driving.speedActual`, `driving.speedDisplayed` | km/h (logged/available, not drawn) |
| `navigation.GPSPosition` | `{"GPSPosition":{"latitude":…,"longitude":…}}` — vehicle position for camera matching |
| `navigation.GPSExtendedInfo` | `{"GPSExtendedInfo":{"heading":…}}` — counter-clockwise from North (per BimmerGestalt/AAIdrive), converted clockwise by the plugin; pending on-road verification |

## Speed Cameras

`cameras.txt` in the plugin directory stores one fixed camera per line as `lat lon limit dir`, where `dir` is the monitored direction of travel in degrees clockwise from North, or `-1` for bidirectional monitoring. The file header includes source attribution, `# country`, `# iso`, and `# date`, which populate the **App information** page.

- **Trigger window:** Active from the configured **Warning distance** ahead of a camera until 50 m past it. Requires the engine to be running, suppresses alerts while factory navigation guidance is active, and matches only cameras ahead on the vehicle's path (within 30° of heading and 60 m laterally) that monitor the vehicle's direction of travel (within 50°; clears if turning more than 70° away). Fixed cameras only.
- **Display modes:** When Sport Bar is off, the alert uses the full canvas (camera icon, speed limit, and shrinking distance bar). When Sport Bar is active, a compact camera icon appears between the gear and RPM, with the speed limit shown in the RPM slot.
- **Database maintenance:** `tools/cameras_build.py` builds `cameras.txt` per country from `tools/cameras_sources.json`, fetching only the selected country. Database updates are independent of app releases (see skill `update-speedcam-db`).
  - **Ukraine** (example bundled dataset): Patrol Police fixed-camera registry combined with OpenStreetMap direction tags (bidirectional radar complexes and undirected entries alert in both directions).
  - **United Kingdom** (example OSM dataset): OpenStreetMap `highway=speed_camera` nodes with signed mph limits and left-hand traffic rules (desk-built, not yet road-tested).

## Build and Packaging

```sh
make -C native test                              # Host unit tests (drawing, sender, scenes, app logic, cameras, MOST, diag_link)
sh native/docker/qnx.sh build                    # Worker binaries: native/out/{hud-sportbar-qnx,shift-deck-qnx}
sh native/docker/qnx.sh package                  # Full build: native/out/libExtendedHUD.so, out/ExtendedHUD/, out/extended-hud.zip
sh .claude/skills/deploy-plugins/abi_check.sh out/libExtendedHUD.so
```

`native/build.sh` links against the target head unit's `libAppFramework.A.so` and `libCoreFramework.A.so` placed in `native/frameworks/` (git-ignored; never commit proprietary BMW binaries). Always verify that every imported `bmw::` symbol resolves against the target frameworks before installing (`abi_check.sh` via QNX SDP or `python3 tools/abi_check.py` without the SDK). See [BUILD.md](BUILD.md) for toolchain setup.

## Installation, Update, and Removal

The plugin directory on the HMI node is `/var/opt/hmi/data/ID5/Plugins/ExtendedHUD/`, containing `ExtendedHUD.manifest` (Etch connector **121**), `ExtendedHUD.xml`, `libExtendedHUD.so`, `hud-sportbar-qnx`, `shift-deck-qnx`, `gateway-ip.txt`, `cds.txt`, and `cameras.txt`. See [../INSTALL.md](../INSTALL.md) for full instructions.

- On split-node units, access the directory from the SSH node at `/net/hu-omap/fs/sda1/opt/hmi/data/ID5/Plugins/` and run `chmod` via `on -f hu-omap`. Verify every uploaded file by SHA-256 readback, then restart iDrive (hold the volume knob until the screen goes dark).
- **Updating:** Upload modified files under a temporary `.new` name, verify SHA-256 readback, then `mv` over the existing file so mapped binaries and running workers are never overwritten in place.
- **Removing:** Delete the `ExtendedHUD` directory (and `Plugins` if it did not exist prior to installation) and restart iDrive.
- **Single-sender requirement:** Do not install or run alongside another HUD sender (`HudTest` / HUD Gauges, Wayglass); concurrent HUDTerminal senders freeze the HUD. ExtendedHUD refuses to start if `HudTest` is present.

## Troubleshooting

| Symptom | Diagnostic Steps |
|---|---|
| No bar with engine running | Check gear (hidden in P, N, R) or unmapped gear values (`gear raw` in `/tmp/hud-sportbar.log`, CDS values in `/tmp/hud-cds.log`) |
| No bar while revving in D/S/M | Inspect `/tmp/hud-sportbar.log` (`on -f hu-omap cat /tmp/hud-sportbar.log` on split-node): check `rpm`, `lag`, `transport: MOST` |
| HUD blank while revving | Inspect `/tmp/hud-sportbar.log` for `rpm`, `transport: MOST`, or `Address already in use` |
| Bar flickers off intermittently | Stale drive frames from link latency; review transport logs and staleness threshold |
| Picture appears soft or cropped | Another HUDTerminal image handle was left visible by a prior diagnostic test; clear it with `SetVisibility(h, 0)` |
| No speed-camera warning | Verify engine is running, warning is enabled, camera faces direction of travel, and **App information** lists a valid camera database |

## Known Limitations and Work in Progress

- **CarPlay turn-by-turn navigation (`feature/nav`):** The NBT Evo iAP2 driver (`mm-ipod`) does not request route guidance from iOS by default; enabling CarPlay turn data requires hooking `mm-ipod`, which involves system-level modifications outside the plugin directory.
- **On-road speed-camera validation:** Heading conversion and alert matching are covered by host unit tests; live on-road validation past fixed cameras is pending.
- **Gears 3rd–8th:** CDS `driving.gear` values `7..12` are inferred from the sequential mapping verified for 1st (`5`) and 2nd (`6`) and remain to be confirmed at highway speeds.
