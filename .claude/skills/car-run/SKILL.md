---
name: car-run
description: Use when the owner is at the car (F34 NBT Evo, ENET cable) and wants to run a parked HUD/cluster session — probing the 82/xx terminal, drawing grids/swatches, or running the HUD worker — with hud_car_run.py. Not for desk-only work.
---

# Parked car run (HUD / KOMBI terminal)

One agent loop drives the whole session: you, the owner, and `tools/hud_car_run.py`. Don't spawn
subagents for this. Every step needs the owner's eyes and a yes, and a handoff loses the log context.

## Load on demand (don't read everything up front)
- Safety: `.claude/rules/car-backup-cleanup.md`. Read it before the first write: backup, recovery ladder, cleanup.
- Access: `.claude/rules/car-ssh.md`. HU and terminal facts: `.claude/rules/car-headunit.md` (both are always loaded).
- Today's steps: the newest `car-run-*-plan.md` in `.claude/stories/hud-redesign/`, which is the plan for this run.
- Step IDs and filming notes from run 1: `.claude/stories/hud-redesign/car-session-1.md`, only when a step refers to them.

## Before the car (laptop, no asking)
1. `sh native/docker/qnx.sh build`. Compare the sha256 and size with the run plan and say so if they differ.
2. `make -C native test` and `python3 -B -m unittest discover -s tools -p 'test_*.py'` pass.
3. Open the shared SSH master (car-ssh.md). Check the host key.

## Car state
- Default is **parked, ignition on, engine off**. Probes, grids, swatches and `--draw-parked` need nothing more.
- Say explicitly **"start the engine now"** and why, only when a step needs it: `--draw-live` (real RPM and speed),
  confirming a result with the engine running, or recharging after a battery or energy-saving warning. Say "switch it off" afterwards.
  Engine running means parked, P, handbrake on, outdoors.
- A battery or energy-saving warning, or a HUD that goes dark by itself, gets an `abort` and is not a draw result. Put the car state into every `note`.

## The gate (the tool enforces it, you keep to it)
- **Read-only actions never ask.** That covers capture, inventory, size probes and get-size (steps marked `readonly` in the tool),
  laptop-side builds and tests, log reading, `plan`, `report`, `note`, `set`, and read-only `ssh` (`ls`, `pidin`, `cat`). Run them and report.
- **Ask only before something that writes or draws**: `stage`, grid, swatches, draw steps, `cleanup`, starting the engine.
  Say in one or two lines what it does, what to film (**cluster and HUD**) and how long it takes. Wait for the owner's "yes",
  then `--confirmed run STEP`. One yes covers one step. `abort` never asks.
- **Stop at the first surprise** and `abort`: an rc other than 0, `MOST open failed`, or any `flags` from the log parser.
  `answers dropped` or `timeouts` count. "Accepted, but nothing visible" with clean flags is a **result**: `note` it and go to the next
  step in the plan. Don't explain it away. **Read the raw rx lines before you draw a conclusion.** Run 1 blamed the car for a bug in our sender. Don't retry on your own, and never restart the HU.
- Before each write or draw step, know which recovery rung fixes it (car-backup-cleanup.md). Run A2 before `stage`, because it is the `/tmp` baseline that cleanup checks.
- Never two HUD senders at once. Nothing goes into `Plugins`, no reboot, no coding. Everything runs from `/tmp/hud-run`, which `cleanup` removes.

## Evidence (this is what makes later evals possible)
- Logs stay in `~/nbt-hud-run-<date>/logs/<step>-<n>.log`, timestamped, raw. Copy them into the story's
  `car-run-N/session-<date>/`, but never the VIN, serials or BMW binaries.
- Each new failure mode becomes a replay test in `tools/test_hud_car_run.py` or `native/tests/`.
  Use the real log lines, as `test_replayed_run1_logs_flag_dropped_answers` does.

## After
- Cleanup per car-backup-cleanup.md, also after an abort or a crash. `LEFTOVER` from `cleanup` is a stop: look, ask, remove only our own files.
- `report`, then the results go into the run's README. Distilled facts go into `.claude/rules/car-headunit.md`
  in the same session, marked `[car]`. **Correct** a wrong `[car]` fact with its evidence; don't leave it standing.
- Write the next `car-run-N+1-plan.md` and point the story README at it.

## Learned in runs 4–6 (2026-11-18)
- **Asks are one line** ("U1 next: strip with dim 5, 40 s, film HUD. ok?"). The owner said briefings waste tokens. Report results just as briefly.
- **Read the raw lines:** `python3 .claude/skills/hud-terminal/decode_log.py LOG` after every step with `--log`. Run 4's T0 "unanswered
  call" was an op F without a handle, and the "392 timeouts" were our own sender recounting a stale entry.
- **Undo what a step switched on.** `SetVisibility 1` (grids on 33/34/37/46, text areas 0x1A/0x1B) and the pen state survive our process and
  an iDrive restart until the HUD sleeps. Run 5 chased a "shade" through two colour redesigns before finding the T1 grids' leftovers.
  End every grid or text step with `--hide …` / `--text-hide`.
- **The design is frozen** (memory `hud-design-frozen`). Don't change the look to fix a symptom; find the cause first.
- **ExtendedHUD is installed:** before any manual worker run, the owner unticks Sport Bar and Display Speed Camera Warning in the menu, or you would be a second
  HUD sender (and `slay` by name kills the app's copy). See the extendedhud skill.
- **SSH drops** when the laptop's link-local address changes or iDrive restarts: reopen the master with keepalive (car-ssh.md) and check the
  unit before retrying. Nothing ran.
- Other skills: hud-terminal (protocol), extendedhud (our app), deploy-plugins (writes to Plugins), dme-reads (engine data),
  update-speedcam-db (the camera list). CarPlay turns: `feature/nav` branch.
