---
name: install-extendedhud
description: Guided step-by-step workflow to install, update, or remove ExtendedHUD on a BMW NBT Evo (iDrive 6) vehicle with a factory windscreen HUD over ENET/SSH. Covers node layout detection, ABI compatibility verification via tools/abi_check.py, pre-install inventory, SHA-256 readback upload, chmod, iDrive restart, menu verification, atomic updates, rollback, and speed-camera database setup. Use for user-guided installation (e.g., via docs/AGENT-INSTALL-PROMPT.md). For developer build/deploy workflows, see deploy-plugins.
---

# Guided ExtendedHUD Installation

Follow `INSTALL.md` together with the operational safeguards below to keep the target vehicle's head unit safe.

## Ground Rules

- **Minimal persistent footprint:** Write only inside `.../Plugins/ExtendedHUD` (and create `Plugins/` if absent). Perform no coding, no ECU writes, no `authorized_keys` changes, no system file modifications, and no remote reboot commands. The user restarts iDrive manually via the volume knob.
- **Read-only first, explicit approval for every write:** Connecting, listing directories, hashing, and copying the framework libraries locally need no confirmation. Before **every write** (`mkdir`, upload, `chmod`, `mv`, `rm`), state clearly what will be written where and wait for explicit user approval.
- **Secret and binary hygiene:** Never print, store, or commit the root password, vehicle VIN, or proprietary BMW binaries (`libAppFramework.A.so`, `libCoreFramework.A.so`). Pass the SSH password via an environment variable or SSH askpass helper, never in chat or command history.
- **Stop on anomalies:** If anything unexpected occurs (unknown node layout, missing ABI symbol, SHA-256 mismatch, or a conflicting HUD app such as `HudTest`), stop immediately and explain clearly rather than improvising.
- **One step at a time:** The user is at the vehicle; keep each prompt concise and state what they should expect to see.

## 1. Pre-Flight Discovery (Read-Only)

| Check | Command / Action | Stop Condition |
|---|---|---|
| SSH reachable | `nc -z <HEAD_IP> 22` after `ping -b <IF> 169.254.255.255` (standard NBT Evo Jacinto ENET IP is `169.254.199.99`; ENET is link-local so an empty ARP cache looks like "host down") | Port 22 closed: user must unlock SSH with the Bavarian SSH Client |
| Legacy SSH options | `-o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa -o KexAlgorithms=+diffie-hellman-group14-sha1`; use a single ControlMaster | — |
| Node layout | `ls /net`: presence of `hu-omap` indicates split-node; otherwise single-node | Unexpected layout: stop and inspect |
| Firmware | `cat /etc/version 2>/dev/null; uname -a`; record locally | — |
| HMI node tools | Split-node: `on -f hu-omap ls /bin/chmod`; single-node: `ls /bin/chmod` | — |
| Conflicting HUD apps | `ls <Plugins>`: check for `HudTest` (HUD Gauges) or other HUD senders | Present: concurrent HUD senders freeze the HUD; remove first or abort |
| Factory HUD | Confirm the vehicle has a factory windscreen HUD displaying speed | No factory HUD: nothing to render to; stop |

Target paths (split-node paths as seen from the SSH node; single-node paths are `[inferred]`):

| Layout | Plugin Directory | BMW Frameworks Directory |
|---|---|---|
| Split-node | `/net/hu-omap/fs/sda1/opt/hmi/data/ID5/Plugins/ExtendedHUD` | `/net/hu-omap/fs/sda0/repository/istep/opt/hmi/ID5/lib/` |
| Single-node | `/var/opt/hmi/data/ID5/Plugins/ExtendedHUD` | `/opt/hmi/ID5/lib/` |

## 2. Package & ABI Compatibility Check (Read-Only)

1. Obtain `extended-hud.zip` (from Releases or built via `docs/BUILD.md`), unpack into a clean directory, and verify `shasum -a 256 -c SHA256SUMS`.
2. Copy `libAppFramework.A.so` and `libCoreFramework.A.so` from the head unit into a temporary or git-ignored local directory.
3. Run `python3 abi_check.py ExtendedHUD/libExtendedHUD.so libAppFramework.A.so libCoreFramework.A.so` (bundled in the zip and at `tools/abi_check.py`):
   - `bmw symbols ok, pluginAppInfo=1`: safe to proceed.
   - `MISSING`: stop. The binary will not load on this firmware; offer to rebuild from source against these frameworks (`docs/BUILD.md`, requires the QNX SDP 6.5 Docker toolchain) and list the missing symbols.
4. Worker binaries (`hud-sportbar-qnx`, `shift-deck-qnx`) depend only on standard QNX runtime libraries and dynamically loaded DSI; no framework ABI check is needed.

## 3. Installation (Confirm Each Write Step)

1. Save a pre-install listing locally: `ls -laR <Plugins>` (or record that `Plugins/` did not exist).
2. `mkdir -p <app folder>`.
3. Upload every file in `ExtendedHUD/` using `cat > <app folder>/<name>` (NBT Evo units do not provide `scp`).
4. Set execute bits on `hud-sportbar-qnx` and `shift-deck-qnx`:
   - Split-node: `on -f hu-omap chmod 755 /var/opt/hmi/data/ID5/Plugins/ExtendedHUD/<file>`.
   - Fallback if `chmod` is unavailable: `rm -f <file>; cp /bin/cat <file>` (inherits `0555`), then stream real contents via `cat > <file>`.
5. Stream every uploaded file back and verify its SHA-256 matches the local package. Re-upload on mismatch; stop if a mismatch persists.
6. Save a post-install listing (`ls -laR`).
7. Instruct the user to hold the iDrive volume knob for ~30 s until the display powers off. Expect the SSH session to drop; reconnect afterward.

## 4. Verification

- Confirm **My Vehicle → ExtendedHUD** appears and **App information** displays the release version and `Speed cameras: <ISO>-<count>-<date>` (e.g. `UA-426-2026-11-19`).
- Enable **Show Sport Bar** or **Show camera warning** (in their submenus) and verify the status line transitions to `Running…`.
- Inspect `/tmp/hud-sportbar.log` read-only (`on -f hu-omap cat /tmp/hud-sportbar.log` on split-node; `cat /tmp/hud-sportbar.log` on single-node). `82/02 up` confirms HUDTerminal connectivity; `MOST open failed` indicates an incompatible HUD terminal — uncheck all options and stop.
- Verify **Sport Bar** with engine running, gearbox in D/S/M, and Sport/Sport+ mode (or **Always show Sport Bar** enabled). Remaining hidden in P/N/R or with engine off is expected.
- **Recovery from display glitches:** Uncheck all ExtendedHUD options (worker clears its canvas) and restart iDrive. If artifacts remain, switch ignition off, lock the vehicle, and allow the head unit/KOMBI to sleep; no HUD state persists past sleep.

## 5. Speed-Camera Database

The release package bundles a default camera database (currently Ukraine). To refresh it or build a database for another country (for example, United Kingdom), use skill `update-speedcam-db`, which fetches only the requested country from `tools/cameras_sources.json` and replaces `cameras.txt`. Note that entries without a speed limit never alert, and entries without a direction alert in both directions.

## Update and Removal

- **Update:** Upload each modified file as `<name>.new`, verify SHA-256 readback, then atomically `mv` it over the target (never overwrite mapped `libExtendedHUD.so` or a running worker in place). Follow with an iDrive knob restart; `settings.txt` is preserved.
- **Remove:** `rm -rf <app folder>` (plus `<Plugins>` if absent in the pre-install listing), then perform an iDrive knob restart. If `HmiMain3D` enters a restart loop, SSH remains available for removal.

## Reference Platform Notes

- **Tested configuration:** 2018 BMW F34 340i (B58, 8-speed automatic), NBT Evo (iDrive 6, firmware `NBTevo_X20511I`, split-node `hu-jacinto` / `hu-omap`), 6WB digital cluster + factory windscreen HUD.
- **Identifiers & ports:** Etch connector `121`, worker UDP port `42873` (avoids ShiftDeck's `42863`), MOST `HUDTerminal` FBlock `0x82` instance `2` (handles `31`/`32`, container `23`). Other 6WB + factory HUD vehicles likely match (`[inferred]` until verified on target hardware).
- **Runtime files & privacy:** Writes only `settings.txt` and transient `cds.guard` (cleared 30 s after startup) inside its plugin folder, plus diagnostic logs under `/tmp` (RAM). Operates completely offline and transmits no data off the vehicle.
