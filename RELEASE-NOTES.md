# Release notes

Versions follow `MAJOR.MINOR.PATCH`: PATCH for fixes and small menu/text changes, MINOR for a new feature or a new setting,
MAJOR once the app is stable on more than one car. The version shows in iDrive under **ExtendedHUD → App information**
(`HUD_APP_VERSION` / `HUD_APP_RELEASED` in `native/src/hud/hud_app.h`). The camera list has its own date and is updated without
a release.

## 0.0.1 (2026-11-20)

First release. Installed and checked on the reference car (F34 340i, NBT Evo `NBTevo_X20511I`, 6WB cluster, factory HUD).

**Sport Bar**
- M-style sequential rev bar on the factory HUD, next to the factory speed: the bar fills from 0, gear on the left, RPM on the
  right. White on dim grey by day; the HUD tints it orange at night by itself.
- About 6 frames a second (pipelined MOST calls, ~160 ms a frame).
- Revs, gear and driving mode from the car data service (CDS). The gear shows in D/S/M only and is never estimated.
- Shown in Sport and Sport+, or in every mode with **Always show Sport Bar**.
- **Shift point** 6,200–6,600 rpm: past it the gear digit blinks at a fixed rate.

**Speed-camera warning**
- Camera icon, limit and a distance bar, from 300/500/800 m (**Warning distance**) before a fixed camera to 50 m past it.
  With the Sport Bar on, a camera badge next to the gear and the limit in place of the revs.
- Uses the car's own GPS position and heading. Warns only for cameras that watch your direction of travel, and clears when you
  turn away.
- Optional **Orange camera warning**.
- Camera list: Ukraine, 426 fixed cameras (Patrol Police list, directions from OpenStreetMap), data from 2026-11-19. A builder
  for other countries is included (`tools/cameras_build.py`; the United Kingdom from OpenStreetMap, not checked on a car).

**iDrive menu** (My Vehicle → ExtendedHUD)
- Main page: status line and three submenus: **Sport Bar**, **Speed Camera Warning**, **App information**. Options that need
  their parent ticked grey out.
- App information: `Version 0.0.1, released 2026-11-20` and `Speed cameras: UA-426-2026-11-19`.

**Known limits**
- The heading direction from CDS (counter-clockwise, flipped to clockwise in the plugin) is taken from BMW's Connected apps and
  not yet confirmed on a drive: if east/west cameras never warn, report it.
- 40 cameras that OpenStreetMap marks as watching both ways, and 8 with no known direction, warn in both directions.
- CarPlay turn-by-turn navigation is not in this release (branch `feature/nav`).
