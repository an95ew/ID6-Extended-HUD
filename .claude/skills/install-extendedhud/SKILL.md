---
name: install-extendedhud
description: Install, update or remove ExtendedHUD on SOMEONE ELSE'S BMW NBT Evo (iDrive 6) with a factory HUD, guiding the owner step by step over ENET/SSH. Covers the compatibility check (node layout, framework symbols via tools/abi_check.py), the before-listing, the copy with hash readback, chmod, the iDrive restart, the first check in the menu, updates by rename, rollback and the speed-camera list. Use when a user (not this repo's maintainer) asks to install, update or remove ExtendedHUD, or pastes docs/AGENT-INSTALL-PROMPT.md. For the maintainer's own car use deploy-plugins.
---

# Install ExtendedHUD on a user's car

The human guide is `INSTALL.md`. Follow it, and stick to these rules. They exist to keep a stranger's head unit safe.

## Ground rules

- **One persistent change only:** the folder `.../Plugins/ExtendedHUD` (and `Plugins` itself if it didn't exist). No coding, no ECU
  writes, no `authorized_keys`, no edits to system files, no reboot commands. The user restarts iDrive with the volume knob.
- **Read-only first.** Connecting, listing, hashing and copying the frameworks to the laptop need no confirmation. **Every write**
  (mkdir, upload, chmod, mv, rm) needs the user's explicit yes for that step, after you've said exactly what will be written where.
- **Never print, store or commit the root password**, the VIN or BMW binaries (framework libraries). Keep the password in an
  environment variable or SSH askpass, never in a command line you show or a file in the repo.
- If anything is unexpected (an unknown layout, a symbol MISSING, a hash mismatch, a `HudTest` folder), **stop and explain**. Don't
  improvise around it.
- Talk in short steps. The user is in or next to the car: one action at a time, say what they should see.

## 1. Facts to collect (read-only)

| Fact | How | Stop if |
|---|---|---|
| SSH works | `nc -z HEAD 22` after `ping -b IF 169.254.255.255` (ENET is link-local; an empty ARP table looks like "host is down") | closed: the user must unlock SSH with the Bavarian SSH Client |
| Legacy algorithms | `-o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa -o KexAlgorithms=+diffie-hellman-group14-sha1`; one ControlMaster connection | |
| Node layout | `ls /net` lists `hu-omap` → split node; else single node | something else: unknown, stop |
| Firmware | `cat /etc/version 2>/dev/null; uname -a`; record it | |
| HMI node tools | split: `on -f hu-omap ls /bin/chmod`; single: `ls /bin/chmod` | |
| Other HUD apps | `ls <Plugins>`: `HudTest` (HUD Gauges) or any other HUD sender | present: two HUD senders freeze the HUD; the user removes it first or stops |
| Factory HUD | ask the user: does the windscreen HUD show speed? | no HUD: nothing to draw on, stop |

Paths (split node from the SSH node; single node `[inferred]`, not seen on a car yet):

| | App folder | Frameworks |
|---|---|---|
| split | `/net/hu-omap/fs/sda1/opt/hmi/data/ID5/Plugins/ExtendedHUD` | `/net/hu-omap/fs/sda0/repository/istep/opt/hmi/ID5/lib/` |
| single | `/var/opt/hmi/data/ID5/Plugins/ExtendedHUD` | `/opt/hmi/ID5/lib/` |

## 2. Compatibility (read-only)

1. Get the package: the release `extended-hud.zip`, or build it (`docs/BUILD.md`). Unzip it into a fresh folder and run
   `shasum -a 256 -c SHA256SUMS`.
2. Copy `libAppFramework.A.so` and `libCoreFramework.A.so` from the car into a git-ignored or temporary folder.
3. `python3 abi_check.py ExtendedHUD/libExtendedHUD.so libAppFramework.A.so libCoreFramework.A.so` (also `tools/abi_check.py`).
   - `bmw symbols ok, pluginAppInfo=1`: go on.
   - `MISSING`: the library won't load on this firmware. Stop. Offer a rebuild against these frameworks (`docs/BUILD.md`, needs the QNX
     SDP 6.5 Docker image) and report the missing symbols.
4. The worker binaries need only `libc`, `libm`, `libsocket` and the DSI library the unit already has. No check needed.

## 3. Install (writes: ask before each)

1. Save a before-listing to the laptop: `ls -laR <Plugins>` (or note "Plugins absent").
2. `mkdir -p <app folder>`.
3. Upload each file of `ExtendedHUD/` with `cat > <app folder>/<name>` (no `scp` on these units).
4. Executables `hud-sportbar-qnx`, `shift-deck-qnx`: `chmod 755` (split: `on -f hu-omap chmod 755 /var/opt/hmi/data/ID5/Plugins/ExtendedHUD/<f>`).
   No `chmod` anywhere: `rm -f X; cp /bin/cat X` (inherits 0555), then `cat > X` with the real contents.
5. Read every file back and compare its sha256 with the package's. Any mismatch: re-upload that file; still bad, stop.
6. Save an after-listing.
7. The user holds the volume knob ~30 s until the screen goes dark. The SSH connection drops; reconnect afterwards.

## 4. First check

- **My Vehicle → ExtendedHUD** is there. **App information** shows the version and "Speed cameras: <country>, <n>, data from <date>".
- Status line "Running…" once Sport Bar or the camera warning is ticked.
- Log (read-only): split `on -f hu-omap cat /tmp/hud-sportbar.log`, single `cat /tmp/hud-sportbar.log`. `82/02 up` = the HUD link
  works. `MOST open failed` = this unit's HUD terminal differs: untick everything and report it.
- Sport Bar: engine running, gearbox in D/S/M, Sport mode (or tick Always). Nothing in P/N/R or with the engine off is correct.
- If the HUD shows garbage or freezes: untick everything (the worker clears its picture), then restart iDrive. If that's not
  enough, ignition off, lock the car and wait until the head unit sleeps. Nothing of ours survives that.

## 5. The camera list

The shipped list is Ukraine's. For another country, or to refresh it, use skill `update-speedcam-db`: it fetches only that
country's sources (`tools/cameras_sources.json`) and swaps just `cameras.txt` on the car. A country not in the file can be added
there. Tell the user: cameras without a limit never alert, and cameras without a direction alert both ways.

## Update and remove

- **Update:** upload each changed file as `<name>.new`, verify its hash, then `mv` it over the old one. Never `cat >` over the
  installed `libExtendedHUD.so` or a running program (the HMI has it mapped). Then a knob restart. `settings.txt` stays.
- **Remove:** `rm -rf <app folder>` (and `<Plugins>` if the before-listing said it was absent), then a knob restart. If the HMI keeps
  restarting, SSH still works: remove the folder the same way.

## Facts (from the tested car: F34 340i, NBT Evo X20511I, split node)

- App connector id 121. Worker UDP port 42873 (ShiftDeck's 42863 stays free). The HUD is MOST FBlock 0x82 instance 2, images 31/32,
  container 23. Other cars with a 6WB and a factory HUD probably match, but that's `[inferred]` until someone reports it.
- Files the app writes itself: `settings.txt` (menu ticks) and `cds.guard` (for 30 s at start) in its folder, logs in `/tmp` (RAM).
- The app needs no internet, and it never sends anything off the car.
