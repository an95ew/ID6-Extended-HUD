---
name: car-run
description: Run a parked HUD/cluster diagnostic session on a BMW NBT Evo (iDrive 6) vehicle over ENET/SSH using tools/hud_car_run.py — probing the MOST 82/xx terminal, drawing grids or palette swatches, or running the HUD worker interactively. Not for desk-only work.
---

# Parked Vehicle Test Session (HUD / KOMBI Terminal)

Drive the entire session in a single agent loop: the agent, the operator at the vehicle, and `tools/hud_car_run.py`. Do not spawn subagents for live sessions — every write or draw step requires the operator's direct visual verification and explicit approval, and context handoffs lose session log state.

**Reference test platform:** 2018 BMW F34 340i (B58, 8-speed automatic), NBT Evo (iDrive 6, firmware `NBTevo_X20511I`, split-node `hu-jacinto` / `hu-omap`), 6WB digital cluster with factory windscreen HUD.

## Load on Demand

- Safety, backup, and recovery rules: local `.claude/rules/car-backup-cleanup.md` (if present) and `INSTALL.md`. Review recovery steps before any write.
- SSH connection settings and head-unit reference notes: local `.claude/rules/car-ssh.md` and `.claude/rules/car-headunit.md` (if configured), or `INSTALL.md` § Connect.
- Session plan: the active run-plan document (if one was prepared for the session).

## Pre-Session Preparation (Laptop, No Confirmation Required)

1. Build QNX worker binaries: `sh native/docker/qnx.sh build`. Compare the SHA-256 checksum and file size against the session plan and report any mismatch.
2. Verify all host tests pass:
   - `make -C native test`
   - `python3 -B -m unittest discover -s tools -p 'test_*.py'`
3. Open the shared SSH ControlMaster connection and verify the host key.

## Vehicle State Rules

- **Default state:** Parked, ignition on, engine off. Terminal probes, grids, palette swatches, and `--draw-parked` require nothing more.
- **Engine running:** Explicitly instruct **"start the engine now"** and state the reason only when a step requires it (e.g., `--draw-live` with live RPM/speed, verifying engine-running behavior, or recharging after a low-battery/energy-management warning). Always confirm **"switch the engine off"** afterward. Engine-running tests must be performed parked, in P, handbrake engaged, outdoors.
- **Abort condition:** Any battery or energy-management warning, or a HUD that powers down unexpectedly, triggers an immediate `abort` and does not count as a valid draw result. Record the vehicle state in every session `note`.

## Execution Gate

- **Read-only actions (no confirmation needed):** Evidence capture, node inventory, size probes (`readonly` steps in `hud_car_run.py`), laptop builds/tests, log inspection, `plan`, `report`, `note`, `set`, and read-only `ssh` commands (`ls`, `pidin`, `cat`). Execute and report concisely.
- **Write or draw actions (explicit confirmation required):** `stage`, grids, swatches, draw steps, `cleanup`, and starting the engine. Give a concise 1–2 line summary of what will run, what to film (**cluster and HUD**), and the duration. Wait for an explicit "yes" before running `--confirmed run STEP`. One approval covers one step. `abort` never requires confirmation.
- **Stop on first anomaly:** Trigger `abort` on any non-zero exit code, `MOST open failed`, or parser error flags (`answers dropped`, `timeouts`, OpType `9`/`F`). "Accepted, but nothing visible" with clean flags is a valid experimental result: record it with `note` and proceed. Always inspect raw `rx` lines before drawing conclusions — never retry automatically and never issue a head-unit reboot command.
- **Baseline before staging:** Run `A2` before `stage` so `/tmp` has a clean inventory baseline for `cleanup`.
- **Single-sender & temporary staging only:** Never run two HUD senders concurrently. Do not write to `Plugins/`, reboot the head unit, or perform coding during a diagnostic run. Stage all test artifacts in `/tmp/hud-run`, which `cleanup` removes.

## Evidence and Regression Tests

- Save timestamped raw logs in a private local session directory outside tracked git paths. Never commit or archive VINs, serial numbers, passwords, or proprietary BMW binaries.
- Turn every newly discovered failure mode into a deterministic replay unit test in `tools/test_hud_car_run.py` or `native/tests/` using sanitized log lines.

## Post-Session Wrap-Up

- Run `cleanup`, including after an abort or crash. If `cleanup` reports `LEFTOVER`, stop, inspect, confirm with the operator, and remove only files created during the session.
- Generate `report`, record distilled hardware-verified findings tagged `[verified]`, and correct any outdated facts backed by new evidence.

## Operational Lessons

- **Keep prompts concise:** One-line confirmation prompts (e.g., `"Next: <step>, <duration>, film HUD. Proceed?"`) and brief result summaries keep live sessions efficient.
- **Always decode raw logs:** Run `python3 .claude/skills/hud-terminal/decode_log.py <LOG>` after every `--log` step rather than inferring from summary counts alone.
- **Restore persistent terminal state:** `SetVisibility(h, 1)` (on handles `33`/`34`, `37`, `46` or text areas `0x1A`/`0x1B`) and pen state persist across process exit and iDrive restarts until the HUD/KOMBI sleeps. Always end grid or text tests with `--hide …` / `--text-hide`.
- **Do not alter visual design to mask protocol issues:** Diagnose root causes from raw logs before modifying HUD layouts or palettes.
- **Avoid colliding with installed ExtendedHUD:** Before running a manual HUD worker, have the operator uncheck **Sport Bar** and **Display Speed Camera Warning** in iDrive so no second sender is active (and avoid `slay` by binary name while the plugin is running).
- **Handle SSH drops cleanly:** If the laptop's link-local address changes or iDrive restarts, re-establish the ControlMaster connection with keepalive and verify head-unit state before resuming.
- **Related skills:** `hud-terminal` (protocol details), `extendedhud` (plugin internals), `deploy-plugins` (persistent plugin deployment), `dme-reads` (OBD/MOST engine telemetry), `update-speedcam-db` (speed-camera database updates).
