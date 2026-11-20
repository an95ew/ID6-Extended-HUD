# ID6-Extended-HUD

Standalone BMW NBT Evo (iDrive 6) plugin and native QNX HUD worker that draws an M-style sequential rev bar (**Sport Bar**) and a
**speed-camera warning** on the factory windscreen HUD of 6WB cars. It appears as **My Vehicle → ExtendedHUD**.

Tested on an F34 340i (B58), NBT Evo firmware `NBTevo_X20511I`, split-node unit (`hu-jacinto` SSH / `hu-omap` HMI).
Release **1.0.0** (2026-11-20).

**Install it on your car:** [INSTALL.md](INSTALL.md), or let your AI agent do it with you: [docs/AGENT-INSTALL-PROMPT.md](docs/AGENT-INSTALL-PROMPT.md).

Highlights:
- About **6 frames a second** on the factory HUD (pipelined MOST calls, ~160 ms a frame).
- Car data from the HMI's car data service (CDS): revs every ~100 ms, the real gear (shown in D/S/M, hidden in P/N/R) and the driving
  mode (Sport/Sport+ only unless "Always" is ticked). Never estimated.
- Gear blinks past a configurable Shift point (6,200–6,600 rpm).
- **Speed-camera warning:** the camera, its limit and a shrinking distance bar, from 300/500/800 m before a fixed camera to 50 m past
  it, only for cameras that watch your direction of travel. Uses the car's own GPS; works with or without a phone. Optional orange.
- **App information** in the menu: the version and the camera list's country and data date. The list updates without a release
  ([skill `update-speedcam-db`](.claude/skills/update-speedcam-db/SKILL.md)); sources are grouped per country
  ([tools/cameras_sources.json](tools/cameras_sources.json)): Ukraine (Patrol Police + OpenStreetMap) and the United Kingdom (OpenStreetMap).

CarPlay turn-by-turn navigation on the HUD is in development on the [`feature/nav`](https://github.com/an95ew/ID6-Extended-HUD/tree/feature/nav) branch and is not part of this build.

## Special thanks

Special thanks to [inotbarnacle](https://github.com/inotbarnacle), the author of [ID6_apps_prerelease](https://github.com/inotbarnacle/ID6_apps_prerelease/tree/main). His findings and documentation were used to build and provide this plugin.

## Photos

![Sport Bar HUD preview](photos/hud-preview.png)

## What is included

```text
ID6-Extended-HUD/
├── README.md
├── INSTALL.md                       # Install on your car (by hand or with an agent)
├── LICENSE                          # MIT (+ third-party/idrive7-apps-LICENSE)
├── docs/
│   ├── AGENT-INSTALL-PROMPT.md      # Prompt to paste into Claude Code for an install or a camera update
│   ├── EXTENDEDHUD.md               # Architecture, HUDTerminal 82/02 map, menu, speed cameras, troubleshooting
│   ├── HUD-DESIGN.md                # Factory HUD canvas, palette, latency, and Sport Bar design rules
│   ├── BUILD.md                     # QNX SDP 6.5 Docker build, ABI verification, packaging
│   └── MOST.md                      # HSFZ-to-MOST UDS tunnel fallback details
├── native/
│   ├── Makefile                     # Host tests (C99 + Python)
│   ├── build.sh                     # Full QNX build (libExtendedHUD.so + workers)
│   ├── build_hud.sh                 # Framework-free QNX build (workers)
│   ├── package.sh                   # Assembles standalone out/ExtendedHUD/ + SHA256SUMS + out/extended-hud.zip
│   ├── docker/                      # QNX SDP 6.5 x86 Docker toolchain wrapper
│   ├── plugin/                      # ExtendedHUD.manifest (connector 121), ExtendedHUD.xml, cds.txt, cameras.txt, gateway-ip.txt
│   ├── sdk/                         # Minimal BMW RHMI SDK list header
│   ├── src/                         # extended_hud.cpp, hud/* (Sport Bar, cameras), most/*, read-only OBD collector
│   ├── tests/                       # Host unit tests + fake DSI/DME harness
│   └── tools/                       # desk HTML previewer, ABI auditor
├── tools/                           # cameras_build.py + cameras_sources.json (camera lists per country), abi_check.py,
│                                    # hud_car_run.py (parked car-run gate), capture_headunit.py, unit tests
└── .claude/skills/                  # Agent skills: install-extendedhud, update-speedcam-db, HUD, car runs, DME reads, deployment
```

## Architecture

```text
HmiMain3D ── libExtendedHUD.so        My Vehicle → ExtendedHUD menu, settings.txt, worker lifecycle
               │ spawn + SIGTERM
               ▼
             hud-sportbar-qnx         Draws to factory HUD over MOST FBlock 0x82 (HUDTerminal) inst 2
               │ posix_spawn                    ▲ UDP 127.0.0.1:42873: revs, gear, mode, GPS (from the plugin's CDS feed)
               ▼
             shift-deck-qnx           Read-only OBD 01 0C (RPM) & 01 0D (speed) → DME 0x12 (HSFZ or MOST), the fallback
```

The camera warning matches the car's GPS position and heading (CDS `navigation.GPSPosition`, `navigation.GPSExtendedInfo`) against
`cameras.txt` in the app folder every 200 ms, with the engine running and never over the factory navigation's own guidance.

- **Read-only diagnostics:** `shift-deck-qnx` sends only OBD service `01` (`01 0C` RPM, `01 0D` speed) and never writes to any ECU.
- **No HMI-thread blocking:** All drawing and diagnostics run in dedicated child processes outside `HmiMain3D`.
- **Single-sender safety:** Do not install or run alongside another HUD sender (`HudTest` / HUD Gauges, Wayglass); two HUDTerminal senders freeze the HUD. ExtendedHUD refuses to start when `HudTest` is present.

## Host tests and desk preview (no car or QNX SDK needed)

```sh
make -C native test
python3 -B -m unittest discover -s tools -p 'test_*.py'
python3 .claude/skills/extendedhud/check_app.py
python3 native/tools/hud_preview.py -o /tmp/hud-preview.html
```

## Build and package for QNX (NBT Evo)

Building `libExtendedHUD.so` requires QNX SDP 6.5 (ARM GCC 4.4.2) and your head unit's own `libAppFramework.A.so` and `libCoreFramework.A.so` placed in `native/frameworks/` (git-ignored; never commit BMW binaries). See [docs/BUILD.md](docs/BUILD.md) for Docker toolchain setup.

```sh
# Framework-free QNX workers only (hud-sportbar-qnx, shift-deck-qnx):
sh native/docker/qnx.sh build

# Full QNX build + self-contained ExtendedHUD/ bundle & zip with SHA256SUMS:
sh native/docker/qnx.sh package

# Verify every bmw:: symbol resolves against your car's frameworks before installing (QNX SDK, or pure Python):
sh .claude/skills/deploy-plugins/abi_check.sh out/libExtendedHUD.so
python3 tools/abi_check.py native/out/libExtendedHUD.so native/frameworks/libAppFramework.A.so native/frameworks/libCoreFramework.A.so
```

`native/package.sh` validates that every QNX binary is an ARM ELF32 binary, stages `native/out/ExtendedHUD/`, and creates `native/out/extended-hud.zip` with verified `SHA256SUMS`.

## Install, update, and remove

See [INSTALL.md](INSTALL.md): compatibility check, hash-readback install, update by rename, rollback and troubleshooting.
To refresh the speed cameras (any time, no new release): [skill `update-speedcam-db`](.claude/skills/update-speedcam-db/SKILL.md).

## Documentation

- [docs/EXTENDEDHUD.md](docs/EXTENDEDHUD.md) — runtime overview, HUDTerminal `82/02` handles, menu behavior, install & troubleshooting
- [docs/HUD-DESIGN.md](docs/HUD-DESIGN.md) — hardware constraints, palette indices, frame latency, and Sport Bar design rules
- [docs/BUILD.md](docs/BUILD.md) — QNX SDP 6.5 Docker build, framework ABI checks, and packaging
- [docs/MOST.md](docs/MOST.md) — HSFZ-to-MOST UDS tunnel fallback behavior and host test modes

## License and credits

- **MIT License** ([LICENSE](LICENSE), plus [third-party/idrive7-apps-LICENSE](third-party/idrive7-apps-LICENSE)): the whole of this
  build: plugin shell, HUD drawing, speed-camera matcher, MOST/DSI transport, diagnostic link, read-only OBD collector, tools and docs.
  The MOST/DSI transport, diagnostic link and RHMI plugin header foundation came from the MIT-licensed `ID6_apps_prerelease` project.
- **Camera data:** Ukraine's list is the Patrol Police's public table of fixed cameras; directions and the UK list come from
  OpenStreetMap, © OpenStreetMap contributors, [ODbL](https://opendatacommons.org/licenses/odbl/). `cameras.txt` names its sources
  in its header.
- The CarPlay navigation code on the `feature/nav` branch carries its own licenses (PolyForm Noncommercial 1.0.0 for the
  route-guidance part, ISC for the Mapbox glyph geometry). None of it is in this build or its package.
