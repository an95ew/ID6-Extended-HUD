---
name: deploy-plugins
description: Install, update or remove the ExtendedHUD iDrive plugin folder (manifest, xml, libExtendedHUD.so, hud-sportbar-qnx, shift-deck-qnx, gateway-ip.txt) on the owner's split-node NBT Evo (firmware X20511I, HMI on hu-omap, SSH on hu-jacinto): framework check, rebuilding against the car's own libAppFramework/libCoreFramework, the ABI symbol check, packaging, staging, Qnet paths, chmod, hash readback, swap-by-rename, the knob restart and rollback. Use for anything that writes into /var/opt/hmi/data/ID5/Plugins/ExtendedHUD.
---

# Deploying iDrive apps to this car

Writing into `Plugins` is the **only persistent change** we make to the head unit. Every such write needs the owner's request for that
app, a before-listing, a hash-verified readback, and a known rollback (`.claude/rules/car-backup-cleanup.md`). Access and the password:
`.claude/rules/car-ssh.md`. Facts: `.claude/rules/car-headunit.md` § "Installed apps".

## Where things are (split node)

| What | hu-omap path | From hu-jacinto (SSH lands here) |
|---|---|---|
| Plugin folder (persistent qnx6 flash) | `/var/opt/hmi/data/ID5/Plugins` (`/var` → `/fs/sda1`) | `/net/hu-omap/fs/sda1/opt/hmi/data/ID5/Plugins` |
| Framework pair | `/fs/sda0/repository/istep/opt/hmi/ID5/lib/lib{App,Core}Framework.A.so` | `/net/hu-omap/fs/sda0/repository/istep/opt/hmi/ID5/lib/` |
| HMI binary | `/opt/hmi/bin/zipped/HmiMain3D` | `/net/hu-omap/fs/sda0/repository/istep/opt/hmi/ID5/bin/HmiMain3D` |
| RAM scratch | `/tmp` → `/fs/ram5` | `/net/hu-omap/fs/ram5`. jacinto's own `/tmp` = `/dev/shmem`: `mkdir` fails, but `cat > /tmp/a/b` works |

Tools: hu-omap has `chmod`, `cp`, `cat`, `ls`, `sockstat`. hu-jacinto has no `chmod`, `head`, `tail`, `arp` or `ping`. Neither has `cksum`:
verify by reading back to the laptop and hashing there. Run omap commands as `ssh root@169.254.199.99 'on -f hu-omap CMD'`.

HmiMain3D (X20511I) loads `/Plugins/*.manifest` with `{pluginDir}`, the same format as the packages (strings: `/Plugins/`, `.manifest`,
`{pluginDir}`, also `/PluginsCoded`). The app appears after an **iDrive restart: the owner holds the volume knob until the screen goes dark**.

## 1. Check compatibility (read-only, no asking)

```bash
cd native && mkdir -p frameworks
for f in libAppFramework.A.so libCoreFramework.A.so; do
  ssh root@169.254.199.99 "cat /net/hu-omap/fs/sda0/repository/istep/opt/hmi/ID5/lib/$f" > frameworks/$f; done
shasum -a 256 frameworks/*            # X20511I: 86afbba9… / 3130e897…
git check-ignore frameworks/libAppFramework.A.so    # must be ignored: framework binaries never go into a commit
```

Always **rebuild against the target car's own framework pair and check every `bmw::` symbol** before shipping.

```bash
sh native/docker/qnx.sh package                                    # out/ExtendedHUD/ + out/extended-hud.zip + SHA256SUMS
sh .claude/skills/deploy-plugins/abi_check.sh out/libExtendedHUD.so # all bmw:: symbols defined by THIS pair
```

Check a packaged archive with `shasum -a 256 -c SHA256SUMS` after unzipping it into a fresh scratch folder.

## 2. Install a new app folder

```bash
P=/net/hu-omap/fs/sda1/opt/hmi/data/ID5/Plugins
ssh root@169.254.199.99 "ls -laR /net/hu-omap/fs/sda1/opt/hmi/data/ID5" > .claude/stories/<story>/install-<date>/before.txt
ssh root@169.254.199.99 "mkdir -p $P && mkdir $P/App"           # mkdir on the Qnet path works (jacinto's /tmp is the odd one)
for f in App/*; do ssh root@169.254.199.99 "cat > $P/App/$(basename $f)" < $f; done
ssh root@169.254.199.99 'on -f hu-omap chmod 755 /var/opt/hmi/data/ID5/Plugins/App/<each worker>'
for f in App/*; do [ "$(ssh root@169.254.199.99 "cat $P/App/$(basename $f)" | shasum -a 256)" = "$(shasum -a 256 < $f)" ] || echo BAD $f; done
```

Then save `after.txt` (`ls -lR`), tell the owner to do the knob restart, and ask which apps appear.

- Connector ids must be unique across all installed manifests. Known coexisting ID6 apps use 100, 102–104, 111–112, and 120,
  HUD Gauges (HudTest) uses 107, and ExtendedHUD uses **121**. `extendedhud/check_app.py` checks ExtendedHUD's id against the
  reserved list and local manifests.
- ExtendedHUD reads `gateway-ip.txt` from its own folder (default `169.254.21.3`). On this car no gateway answered on ENET, so `shift-deck-qnx`
  runs over the MOST tunnel (see `dme-reads`).
- Never install ExtendedHUD next to `HudTest` (HUD Gauges): two HUD senders freeze the HUD.

## 3. Update a file of an installed app

HmiMain3D has the `.so` mapped, and a worker may be running from its file. **Never write over them in place.** Write a new name, verify
it, then rename it over the old one. The running process keeps the old inode.

```bash
ssh root@169.254.199.99 "cat > $P/App/libApp.so.new" < out/libApp.so
[ "$(ssh root@169.254.199.99 "cat $P/App/libApp.so.new" | shasum -a 256)" = "$(shasum -a 256 < out/libApp.so)" ] \
  && ssh root@169.254.199.99 "mv $P/App/libApp.so.new $P/App/libApp.so"
ssh root@169.254.199.99 'on -f hu-omap chmod 755 …/worker'      # after a worker swap
```

It takes effect at the next knob restart (the library) or the next spawn (a worker).

## 4. Roll back

- One app: `ssh root@169.254.199.99 "rm -rf $P/App"`, then a knob restart.
- Everything: `rm -rf $P`. The folder didn't exist before 2026-11-18 (`install-2026-11-18/before.txt`).
- If HmiMain3D loops, SSH on hu-jacinto stays reachable: remove the folder from there.

## Pitfalls

- The knob restart drops the SSH master (`Permission denied`): reopen it (car-ssh.md) and check the unit before going on.
- `pidin arg | grep NAME` on omap may miss a child process. `on -f hu-omap sockstat` shows who holds which port.
- `slay NAME` kills every process of that name, including an installed app's copy.
- Never copy the password, VIN, framework or HMI binaries into `native/`, `.claude/skills/`, or a commit (`.claude/rules/` and `native/frameworks/` are git-ignored).
