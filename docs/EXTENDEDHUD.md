# ExtendedHUD

ExtendedHUD puts an M-style rev bar ("Sport Bar") and a speed-camera warning on the factory windscreen HUD of an NBT Evo car with
a 6WB cluster. The app shows up as **My Vehicle → ExtendedHUD**. It was tested on an F34 340i (B58), NBT Evo firmware
`NBTevo_X20511I`, on a split-node unit. Release 1.0.0 (2026-11-20). Install: [../INSTALL.md](../INSTALL.md). CarPlay turn-by-turn
navigation is in development on the `feature/nav` branch and is not in this build.

## How it works

```
HmiMain3D ── libExtendedHUD.so        menu, settings file, start/stop of the worker (no drawing, no network in the HMI)
               │ spawn + SIGTERM
               ▼
             hud-sportbar-qnx         draws on the HUD: MOST FBlock 0x82 (HUDTerminal), instance 2
               │ posix_spawn                     ▲ UDP 127.0.0.1:42873, {sampled_ms, rpm, kph}
               ▼
             shift-deck-qnx           OBD 01 0C (RPM) and 01 0D (speed) to the DME, over HSFZ or the MOST tunnel
```

- The plugin writes `settings.txt` in its folder (`sport_bar`, `always`, `shift_rpm`, `blink_rpm`, `cameras`, `cam_dist`,
  `cam_orange`, ...), through a temporary file and a rename. The worker reads it every second.
- The worker only runs while Sport Bar or Display Speed Camera Warning is ticked. It clears its picture when it stops.
- The collector (`native/src/collector.c` → `shift-deck-qnx`) is strictly read-only and sends only OBD service `01`. Its UDP port
  (`42873`) is kept separate from `DRIVE_PORT` (`42863`) so it can coexist with ShiftDeck if installed.
- The car data comes from the HMI's car data service (CDS, below): revs about every 100 ms, the gear and the driving mode on change.
  The collector's OBD read (~260 ms a request over the MOST tunnel) is the fallback while CDS is quiet. Data older than 2 s is not drawn.

## The HUD terminal

The 6WB renders what the head unit sends to the HUDTerminal function block **0x82**: instance **0** is the cluster's navigation picture,
instance **2** the HUD. Calls are OpType 6 with the target handle as the first u16; this car answers with OpType **D**.

| Handle (82/02) | Size | Use |
| --- | --- | --- |
| 31/32 (0x1F/0x20) | 120×72 | ExtendedHUD's double buffer, right of the factory speed |
| 23 (0x17) | | the navigation container: `SetVisibility(0x17, 1)` is needed before anything shows |
| 26/27 (0x1A/0x1B) | | text areas: street and distance in the factory font, Latin and Cyrillic (used by the nav branch only) |

A frame is `ImageRemoveAllItems` on the hidden buffer, a fill colour, polygons, then `SetVisibility` + `SwitchVisibility`. Colours
are palette indices: 2, 4 and 6 are white, 3 dim grey, 5, 8 and 11 orange. At night the HUD tints white toward orange by itself.
Visibility and pen state stay set in the terminal until the HUD sleeps; a test that switches another image on must switch it off
again, or it lies over the picture. The HUD draws a polygon's edges too, so pieces are only merged where they touch.

The look is the split Sport Bar: the whole track always shows dim (index 3), the lit part fills in white (4) like the factory speed,
the gear sits on the left under it and the RPM on the right. It shows only with the engine running and the gearbox in D, S or M,
idle included (before a launch). With the engine off, or in P, N or R, nothing of ours is drawn. Everything comes from the car: the
gear is CDS `driving.gear`, never estimated from RPM and speed. A value not mapped yet counts as "not D/S", so the bar stays off. Past
the Shift point (menu, 6200–6600 rpm, default 6400) only the gear number blinks, one flip per frame.

**Speed.** A frame is at most 12 calls (the bar's pieces merged per colour, each digit its own polygon). A call answers in about
250 ms on this car whatever the window, so the worker overlaps frames (`--pipeline`): the next frame starts as soon as the last one is
all sent, into the other buffer. With `in_flight=32` that is about **6 frames a second (160 ms a frame)**, against ~1.8 s for the
first design. Measured on the car (run 6): window 3 → 828 ms, 8 → 378 ms, 16 → 285 ms, pipelined 32 → 161 ms.

## Menu

| Row | Does |
| --- | --- |
| status line | Off / Starting / Running: bar, camera warning |
| Sport Bar | the rev bar |
| Always show Sport Bar | the bar in every driving mode; greyed out while Sport Bar is off |
| Shift point | a page with five values, 6,200–6,600 RPM: where the gear starts blinking |
| Display Speed Camera Warning | the speed-camera warning (below) |
| Orange camera warning | the camera and its limit in orange (palette 5) instead of white; greyed out while the warning is off |
| Warning distance | a page: 300, 500 (default) or 800 m before the camera; greyed out while the warning is off |
| App information | "Version 1.0.0, released 2026-11-20" and "Speed cameras: Ukraine, 426, data from 2026-11-19" |

Without Always, the bar shows in Sport and Sport+ only. The driving mode comes from CDS `driving.mode`.

Old `settings.txt` lines from earlier builds (`nav`, `lang`, `text`, `street_max`) are ignored. `red=N` (no menu row) sets a fill
index for the gear blink, the 6000+ zone and the camera; 0 (default) keeps white, since the 6WB HUD palette has no red.
`settings.txt` also takes two tuning keys with no menu row: `in_flight=1..32` (HUD calls awaiting an answer, default 32) and `merge_pts=-1..99`
(points per merged polygon; 0 draws one polygon per piece, -1 the layout's default). The worker re-reads the file every second.

**CDS (car data).** `cds.txt` in the app folder (shipped), one `<property> <kind> [interval]` per line (kind `log`, `rpm`, `kph`,
`gear` or `mode`), makes the plugin watch those properties. It logs every value to `/tmp/hud-cds.log` and sends the non-`log` kinds to
the worker. CDS sends a value only when it changes, so the plugin repeats the last one every second. Registering creates `cds.guard`,
and the plugin removes it 30 s later. If a start dies in the CDS calls, the guard stays behind and the next start skips CDS. Delete
`cds.guard` to try again.

| Property (NBT Evo X20511I) | Values |
| --- | --- |
| `engine.RPMSpeed` | `{"RPMSpeed":660}`; ~100 ms updates at interval 50 |
| `driving.gear` | 1 N, 2 R, 3 P, 5 = 1st, 6 = 2nd (seen on the car); 7..12 = 3rd..8th (inferred, same counter) |
| `driving.mode` | 3 Comfort, 4 Sport, 5 Sport+, 7 Eco Pro |
| `driving.speedActual`, `driving.speedDisplayed` | km/h (not drawn) |
| `navigation.GPSPosition` | `{"GPSPosition":{"latitude":…,"longitude":…}}`: the camera warning's position |
| `navigation.GPSExtendedInfo` | `{"GPSExtendedInfo":{"heading":…}}`; the heading counts counter-clockwise from North (per BimmerGestalt/AAIdrive), the plugin turns it clockwise. Not yet confirmed on a drive |

## Speed cameras

`cameras.txt` in the app folder holds one camera per line, `lat lon limit dir`. `dir` is the direction of travel the camera watches,
in degrees from north, or `-1` for both ways. Its header names the source, `# country` and `# date`. The App information page shows
the country and the date, so you can tell how current the list is.

- **When:** from the Warning distance before the camera until 50 m past it. Only with the engine running, never while the factory
  navigation is guiding, and only for a camera ahead on your road (within 30° of your heading and 60 m of your line) that watches your
  direction (within 50°; it drops when you turn more than 70° away). Fixed cameras only.
- **What:** without the Sport Bar, the whole picture: the camera, a limit sign and a distance bar that shrinks to the camera. With
  the bar, a small camera between the gear and the revs, and the limit in the revs' place.
- **The list** is built by `tools/cameras_build.py`, per country from `tools/cameras_sources.json`, fetching only the chosen country.
  It is independent of app releases: the skill `update-speedcam-db` refreshes it and copies just that file to the car.
  - Ukraine: the Patrol Police table (426 fixed cameras on 2026-11-19), directions from OpenStreetMap: 378 directed. 40 are radar
    complexes that control both directions (OSM notes), and 8 are unknown. Both groups alert both ways.
  - United Kingdom: every OSM `highway=speed_camera` (3,495 on 2026-11-20), limits in mph as signed, left-hand traffic. Desk-built
    only, not driven.

## Build and package

```sh
make -C native test                              # host tests: drawing, sender, scenes, app logic, cameras, MOST, diag_link
sh native/docker/qnx.sh build                    # native/out/{hud-sportbar-qnx,shift-deck-qnx}
sh native/docker/qnx.sh package                  # adds native/out/libExtendedHUD.so, out/ExtendedHUD/, out/extended-hud.zip
sh .claude/skills/deploy-plugins/abi_check.sh out/libExtendedHUD.so
```

`native/build.sh` links against the car's own `libAppFramework.A.so` and `libCoreFramework.A.so` in `native/frameworks/` (never committed).
Before installing, check that every `bmw::` symbol the library needs is defined by that pair (`abi_check.sh` / `ntoarm-nm -D`, or
`python3 tools/abi_check.py` without the SDK).
See [BUILD.md](BUILD.md) for QNX Docker toolchain setup.

## Install, update, remove

The app folder is `/var/opt/hmi/data/ID5/Plugins/ExtendedHUD/` on the HMI node: `ExtendedHUD.manifest` (Etch connector **121**),
`ExtendedHUD.xml`, `libExtendedHUD.so`, `hud-sportbar-qnx`, `shift-deck-qnx`, `gateway-ip.txt`, `cds.txt`, `cameras.txt`. Step by step:
[../INSTALL.md](../INSTALL.md). On a split-node unit, reach it from
the SSH node as `/net/hu-omap/fs/sda1/opt/hmi/data/ID5/Plugins/`, and run `chmod` with `on -f hu-omap`. Verify every file by reading
it back, then restart iDrive (hold the volume knob until the screen goes dark).

- To update a file, upload it under a new name, check it, then `mv` it over the old one. The HMI has the library mapped and the worker
  may be running.
- To remove the app, delete the folder and restart iDrive.
- Don't run another HUD app (HUD Gauges / HudTest, Wayglass) at the same time: two senders freeze the HUD. ExtendedHUD refuses to start
  when `HudTest` is installed.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| no bar with the engine running | P, N or R; or the gear value isn't mapped yet (`gear raw` in `/tmp/hud-sportbar.log`, values in `/tmp/hud-cds.log`) |
| no bar while revving in D | `on -f hu-omap cat /tmp/hud-sportbar.log`: `rpm`, `lag`, `transport: MOST` |
| HUD blank while revving | `on -f hu-omap cat /tmp/hud-sportbar.log`: look for `rpm`, `transport: MOST`, `Address already in use` |
| bar flickers off | stale frames: a slow link, see "How it works" |
| picture soft or cropped | another image left visible by an earlier test: `SetVisibility(h, 0)` on it |
| no camera warning | engine off; the warning unticked; the camera watches the other direction; App information: "no list installed" |

## Not done yet

- **CarPlay turns:** on the `feature/nav` branch. The head unit's 2017 iAP2 driver (`mm-ipod`) doesn't ask the iPhone for route
  guidance; getting turns needs a hook inside `mm-ipod`, a system change outside the app folder.
- **Speed cameras on the road:** the heading direction and the alerts are desk-tested; the first drive past a camera is pending.
- **Gears 3rd–8th:** CDS values 7..12 follow the counter seen for 1st and 2nd but haven't been seen on a drive yet.
