---
name: deploy-plugins
description: Developer workflow to build, ABI-verify, install, update, or remove the ExtendedHUD iDrive plugin bundle (manifest, XML, libExtendedHUD.so, hud-sportbar-qnx, shift-deck-qnx, gateway-ip.txt, cds.txt, cameras.txt) on a BMW NBT Evo (iDrive 6) head unit (split-node hu-jacinto/hu-omap or single-node). Covers framework extraction, rebuilding against target libAppFramework/libCoreFramework, ABI symbol checks, Qnet paths, chmod, SHA-256 readback, atomic swap-by-rename, iDrive knob restart, and rollback. Use for persistent writes under /var/opt/hmi/data/ID5/Plugins/ExtendedHUD.
---

# Deploying ExtendedHUD Plugins to NBT Evo

Writing into `Plugins/` is the **only persistent modification** made to the head unit. Every write requires explicit operator approval, a pre-install directory listing, SHA-256 readback verification, and a documented rollback path.

**Reference test platform:** 2018 BMW F34 340i (B58, 8-speed automatic), NBT Evo (iDrive 6, firmware `NBTevo_X20511I`, split-node `hu-jacinto` SSH / `hu-omap` HMI), 6WB cluster + factory windscreen HUD.

## Filesystem Layout (Split-Node NBT Evo)

| Resource | `hu-omap` Local Path | Path from `hu-jacinto` (SSH Entry) |
|---|---|---|
| Plugin directory (persistent qnx6 flash) | `/var/opt/hmi/data/ID5/Plugins` (`/var` → `/fs/sda1`) | `/net/hu-omap/fs/sda1/opt/hmi/data/ID5/Plugins` |
| BMW framework pair | `/fs/sda0/repository/istep/opt/hmi/ID5/lib/lib{App,Core}Framework.A.so` | `/net/hu-omap/fs/sda0/repository/istep/opt/hmi/ID5/lib/` |
| HMI binary | `/opt/hmi/bin/zipped/HmiMain3D` | `/net/hu-omap/fs/sda0/repository/istep/opt/hmi/ID5/bin/HmiMain3D` |
| RAM scratch | `/tmp` → `/fs/ram5` | `/net/hu-omap/fs/ram5` (`hu-jacinto`'s `/tmp` is `/dev/shmem`: `mkdir` fails, though `cat >` works) |

*(Single-node units expose `/var/opt/hmi/data/ID5/Plugins` and `/opt/hmi/ID5/lib/` directly.)*

**Available utilities:** `hu-omap` provides `chmod`, `cp`, `cat`, `ls`, and `sockstat`. `hu-jacinto` lacks `chmod`, `head`, `tail`, `arp`, and `ping`. Neither node has `cksum`, so verify integrity by streaming files back to the laptop and comparing SHA-256 locally. Standard NBT Evo ENET SSH address is `169.254.199.99` (`hu-jacinto`); execute `hu-omap` commands via `ssh root@169.254.199.99 'on -f hu-omap <CMD>'`.

`HmiMain3D` loads `/Plugins/*.manifest` using `{pluginDir}` expansion. Newly installed plugins appear after an **iDrive restart** (hold the volume knob for ~30 s until the display turns off).

## 1. Compatibility Check and Build (Read-Only on Vehicle)

Copy the target head unit's framework libraries into the git-ignored `native/frameworks/` directory and record their SHA-256 digests locally:

```bash
cd native && mkdir -p frameworks
for f in libAppFramework.A.so libCoreFramework.A.so; do
  ssh root@169.254.199.99 "cat /net/hu-omap/fs/sda0/repository/istep/opt/hmi/ID5/lib/$f" > frameworks/$f
done
shasum -a 256 frameworks/*
git check-ignore frameworks/libAppFramework.A.so    # must stay git-ignored
```

Always **rebuild against the target head unit's framework pair and verify every imported `bmw::` symbol** before deployment:

```bash
sh native/docker/qnx.sh package                                     # out/ExtendedHUD/ + out/extended-hud.zip + SHA256SUMS
sh .claude/skills/deploy-plugins/abi_check.sh out/libExtendedHUD.so # verify all bmw:: symbols resolve
```

Verify a packaged archive in a clean scratch folder with `shasum -a 256 -c SHA256SUMS`.

## 2. Install a New Plugin Directory

```bash
P=/net/hu-omap/fs/sda1/opt/hmi/data/ID5/Plugins
ssh root@169.254.199.99 "ls -laR /net/hu-omap/fs/sda1/opt/hmi/data/ID5" > /tmp/id5-before.txt
ssh root@169.254.199.99 "mkdir -p $P && mkdir $P/ExtendedHUD"
for f in native/out/ExtendedHUD/*; do
  ssh root@169.254.199.99 "cat > $P/ExtendedHUD/$(basename "$f")" < "$f"
done
ssh root@169.254.199.99 'on -f hu-omap chmod 755 /var/opt/hmi/data/ID5/Plugins/ExtendedHUD/hud-sportbar-qnx /var/opt/hmi/data/ID5/Plugins/ExtendedHUD/shift-deck-qnx'
for f in native/out/ExtendedHUD/*; do
  [ "$(ssh root@169.254.199.99 "cat $P/ExtendedHUD/$(basename "$f")" | shasum -a 256)" = "$(shasum -a 256 < "$f")" ] || echo "BAD $f"
done
```

Save an after-listing (`ls -lR`), instruct the operator to perform an iDrive volume-knob restart, and confirm **My Vehicle → ExtendedHUD** appears.

- **Connector IDs:** Must be unique across all installed manifests. Known coexisting ID6 apps use `100`, `102–104`, `111–112`, and `120`; `HudTest` (HUD Gauges) uses `107`; ExtendedHUD uses **`121`**. Run `python3 .claude/skills/extendedhud/check_app.py` to validate uniqueness.
- **OBD transport:** ExtendedHUD reads `gateway-ip.txt` from its folder (default `169.254.21.3`). On vehicles where no ENET diagnostic gateway responds, `shift-deck-qnx` automatically falls back to the MOST UDS tunnel (see skill `dme-reads`).
- **Single HUD sender:** Never install ExtendedHUD alongside `HudTest` (HUD Gauges) or Wayglass; two concurrent HUDTerminal senders freeze the HUD.

## 3. Update an Installed File (Atomic Swap)

`HmiMain3D` memory-maps `libExtendedHUD.so`, and workers may be executing from disk. **Never overwrite binaries in place.** Upload to a `.new` filename, verify SHA-256 readback, and atomically rename:

```bash
ssh root@169.254.199.99 "cat > $P/ExtendedHUD/libExtendedHUD.so.new" < native/out/libExtendedHUD.so
[ "$(ssh root@169.254.199.99 "cat $P/ExtendedHUD/libExtendedHUD.so.new" | shasum -a 256)" = "$(shasum -a 256 < native/out/libExtendedHUD.so)" ] \
  && ssh root@169.254.199.99 "mv $P/ExtendedHUD/libExtendedHUD.so.new $P/ExtendedHUD/libExtendedHUD.so"
# If updating a worker binary, restore execute permissions:
ssh root@169.254.199.99 'on -f hu-omap chmod 755 /var/opt/hmi/data/ID5/Plugins/ExtendedHUD/<worker>'
```

Library changes take effect on the next iDrive restart; worker changes take effect on the next worker spawn.

## 4. Rollback

- Remove ExtendedHUD only: `ssh root@169.254.199.99 "rm -rf $P/ExtendedHUD"`, followed by an iDrive knob restart.
- Remove the `Plugins/` directory completely only if your pre-install listing confirmed `Plugins/` did not exist beforehand.
- Even if `HmiMain3D` enters a restart loop, SSH on `hu-jacinto` remains reachable for rollback.

## Operational Pitfalls

- An iDrive knob restart drops any active SSH ControlMaster (`Permission denied`); re-establish the SSH session and verify unit state before continuing.
- `pidin arg | grep <NAME>` on `hu-omap` may omit child processes; use `on -f hu-omap sockstat` to confirm active sockets.
- `slay <NAME>` terminates all processes matching that name, including the installed plugin's running worker.
- Never copy passwords, VINs, proprietary BMW frameworks, or HMI binaries into `native/`, `.claude/skills/`, or any git commit (`.claude/rules/` and `native/frameworks/` remain git-ignored).
