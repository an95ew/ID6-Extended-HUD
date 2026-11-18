# ExtendedHUD

ExtendedHUD puts an M-style rev bar ("Sport Bar") on the factory windscreen HUD of an NBT Evo car with a 6WB cluster. A navigation
scene with turn arrows, lanes and street and distance text is drawn too, but it has no live source yet. The app shows up as
**My Vehicle → ExtendedHUD**. It was tested on an F34 340i (B58), NBT Evo firmware `NBTevo_X20511I`, on a split-node unit.

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

- The plugin writes `settings.txt` in its folder (`sport_bar`, `always`, `nav`, `shift_rpm`, `lang`, `text`, `street_max`), through a
  temporary file and a rename. The worker reads it every second.
- The worker only runs while Sport Bar or Navigation is ticked. It clears its picture when it stops.
- The collector (`native/src/collector.c` → `shift-deck-qnx`) is strictly read-only and sends only OBD service `01`. Its UDP port
  (`42873`) is kept separate from `DRIVE_PORT` (`42863`) so it can coexist with ShiftDeck if installed.
- Over the MOST tunnel one RPM and speed frame takes about 550 ms. The HUD keeps a frame for 2 s before it counts as stale.

## The HUD terminal

The 6WB renders what the head unit sends to the HUDTerminal function block **0x82**: instance **0** is the cluster's navigation picture,
instance **2** the HUD. Calls are OpType 6 with the target handle as the first u16; this car answers with OpType **D**.

| Handle (82/02) | Size | Use |
| --- | --- | --- |
| 31/32 (0x1F/0x20) | 120×72 | ExtendedHUD's double buffer, right of the factory speed |
| 23 (0x17) | | the navigation container: `SetVisibility(0x17, 1)` is needed before anything shows |
| 26/27 (0x1A/0x1B) | | text areas: street and distance in the factory font, Latin and Cyrillic |

A frame is `ImageRemoveAllItems` on the hidden buffer, a fill colour, polygons, then `SetVisibility` + `SwitchVisibility`. Colours
are palette indices: 2, 4 and 6 are white, 3 dim grey, 5, 8 and 11 orange. At night the HUD tints white toward orange by itself. One
call takes about 65 ms with three in flight, so a frame of 20–26 calls takes 1.3–2.1 s. Visibility and pen state stay set in the
terminal until the HUD sleeps; a test that switches another image on must switch it off again, or it lies over the picture.

The look (palette `3,3,3,3,3,3,4`, white at the shift point, no unlit backgrounds, the big layout) was chosen on the car. The strip
starts at 1,000 rpm, so at idle in P the HUD is blank.

## Menu

| Row | Does |
| --- | --- |
| status line | Off / Starting / Running / why it is paused |
| Sport Bar | the rev bar |
| Always show Sport Bar | the bar in every driving mode; greyed out while Sport Bar is off |
| Navigation | the turn scene when a route gives guidance |

The driving mode is not read yet, so the installed default has all three ticked. Otherwise the bar would only show in Sport.

## Build and package

```sh
make -C native test                              # host tests: drawing, sender, scenes, app logic, rg, MOST, diag_link
sh native/docker/qnx.sh build                    # native/out/{hud-sportbar-qnx,shift-deck-qnx,librghook.so}
sh native/docker/qnx.sh package                  # adds native/out/libExtendedHUD.so, out/ExtendedHUD/, out/extended-hud.zip
sh .claude/skills/deploy-plugins/abi_check.sh out/libExtendedHUD.so
```

`native/build.sh` links against the car's own `libAppFramework.A.so` and `libCoreFramework.A.so` in `native/frameworks/` (never committed).
Before installing, check that every `bmw::` symbol the library needs is defined by that pair (`abi_check.sh` / `ntoarm-nm -D`).
See [BUILD.md](BUILD.md) for QNX Docker toolchain setup.

## Install, update, remove

The app folder is `/var/opt/hmi/data/ID5/Plugins/ExtendedHUD/` on the HMI node: `ExtendedHUD.manifest` (Etch connector **121**),
`ExtendedHUD.xml`, `libExtendedHUD.so`, `hud-sportbar-qnx`, `shift-deck-qnx`, `gateway-ip.txt`. On a split-node unit, reach it from
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
| HUD blank, engine at idle | expected: the bar starts at 1,000 rpm |
| HUD blank while revving | `on -f hu-omap cat /tmp/hud-sportbar.log`: look for `rpm`, `transport: MOST`, `Address already in use` |
| bar flickers off | stale frames: a slow link, see "How it works" |
| picture soft or cropped | another image left visible by an earlier test: `SetVisibility(h, 0)` on it |
| navigation never shows | no CarPlay turn source yet (below) |

## Not done yet

- **CarPlay turns.** The head unit's 2017 iAP2 driver (`mm-ipod`) doesn't ask the iPhone for route guidance, so no turns arrive. Getting
  them needs a hook (`librghook.so`) inside `mm-ipod`: declare a route-guidance display, request updates, decode the messages. It is loaded
  through the CarPlay start path. That is a system change outside the app folder and waits for a decision.
- **Driving mode** (Sport / Comfort) is not read.
