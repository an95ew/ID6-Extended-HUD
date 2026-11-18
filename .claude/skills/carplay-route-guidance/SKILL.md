---
name: carplay-route-guidance
description: Get CarPlay turn-by-turn (iAP2 route guidance from Apple Maps / Google Maps) into ExtendedHUD's nav scene on NBT Evo: why the factory mm-ipod never receives it, the identification patch + StartRouteGuidanceUpdates injection that librghook.so does inside mm-ipod, the routeguidance file it writes, how it is loaded (mm-ipod/Connectivity wrappers from /bin/start_screen.sh), and the owner's pending decision. Use whenever the HUD nav shows nothing during a CarPlay route, or before touching mm-ipod, Connectivity or the CarPlay start path.
---

# CarPlay turns for the HUD

**Status (2026-11-18): on hold, the owner's decision is pending.** Before doing anything here, ask which option (below). Full finding:
`.claude/stories/hud-redesign/carplay-nav-source.md`. ExtendedHUD draws the nav scene (`src/hud/hud_nav.c`) only from the demo route
today. In live mode `st.route_active` is never set, so a real CarPlay route shows nothing. That is expected, not a bug.

## Why the phone sends nothing

```
iPhone ──iAP2 (USB/Wi-Fi)──▶ mm-ipod (hu-omap, started per CarPlay session by Connectivity)
                                 identifies WITHOUT a RouteGuidanceDisplayComponent
                                 → the phone never sends 0x5201/0x5202/0x5204
```

The 2017 iAP2 stack in `mm-ipod` ("Media 1.0.70") predates CarPlay route guidance. The phone only sends turns to an accessory that declares
the component and asks for updates. [static; not yet checked on X20511I]

## What a hook has to do (`librghook.so`, preloaded into mm-ipod)

| Step | iAP2 message | Detail |
|---|---|---|
| Find the link | first bytes `ff 5a` / `ff 55` | hooks only `read()`/`write()` on that descriptor; walks `LD_PRELOAD` itself (no useful `RTLD_NEXT` on QNX); never hooks `close()` |
| Declare | `IdentificationInformation` 0x1D01 | add 0x5200/0x5203 to messages sent, 0x5201/0x5202/0x5204 to messages received, and `RouteGuidanceDisplayComponent` (param 30: id 0x0F0E, "Head-Up Display", 256-char limits, 6 maneuvers, lanes); fix lengths and checksums |
| Ask | `StartRouteGuidanceUpdates` 0x5200 | only after **both** `IdentificationAccepted` 0x1D02 and `AuthenticationSucceeded` 0xAA05; takes the next link sequence number, then renumbers the driver's packets (+δ) and shifts the phone's acks back (−δ) |
| Receive | `RouteGuidanceUpdate` 0x5201, `…ManeuverUpdate` 0x5202, `LaneGuidanceInformation` 0x5204 | decode, then hand the driver an empty `PowerUpdate` 0xAE01 with the same sequence: **the 2017 driver leaks every unknown message's buffer and stopped reading after ~20** |
| Publish | `/var/iap/pps/routeguidance` | PPS `key::value` lines via `.tmp` + rename: route_state, dist_to_maneuver(_string,_units), current_road, maneuver_index/type 0..53/exit_angle/driving_side/road/junction_*, lanes `status/highlight/angle,…;…`; removed when mm-ipod starts |

**Loading it:** wrapper scripts shadow `/opt/conn/bin/carplay/mm-ipod` and `/opt/conn/bin/Connectivity` with path-manager links (`ln -sP`,
forgotten at reboot). The links are made at every boot by a `boot.sh` started from **`/bin/start_screen.sh`**, a persistent system change
outside `Plugins`. The installer checks `mm-ipod` by hash (known images 006 and MINI 1703). Do not stop `CONN_MOCCA`
(watchdog reset ~1 min later). Restart with `OnOffDSICommander appreset`.

## The options (as given to the owner)

1. **Recommended:** use only the route-guidance hook (plain `librghook.so`, HUD drawing left off, so there is never a second HUD sender).
   ExtendedHUD reads `/var/iap/pps/routeguidance`.
2. A hook written here that does the same: the same system touch points, much more work.
3. No CarPlay turns: Sport Bar only (the factory nav draws its own arrows).

## When it resumes (option 1)

1. Read-only first: the `mm-ipod` hash on X20511I against the known images (006 and MINI 1703), `/bin/start_screen.sh` as it is now, whether `/var/iap` exists.
   No writes.
2. A plan the owner agrees to (car-backup-cleanup.md): a backup of every file touched, the exact restore, and a test that the restore works.
3. ExtendedHUD side, host-tested: a `routeguidance` parser in `src/hud/` (key::value → `hud_nav_info`), `route_active` from route_state,
   CPManeuverType 0..53 → `hud_maneuver`, lanes → `hud_lane`. The worker polls the file (as it does `settings.txt`). Then `test_hud.c` fixtures
   from a real file.
4. Mind the scene rules: with "Always" ticked the Sport Bar wins. The driving mode is still unknown, so nav needs Always off (`hud_scene.c`).
