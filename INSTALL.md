# Installing ExtendedHUD

ExtendedHUD adds **My Vehicle → ExtendedHUD** to iDrive 6 (NBT Evo). It draws an M-style rev bar (Sport Bar) and a speed-camera
warning on the factory windscreen HUD. This guide is for owners who want it on their own car. You can do it by hand, or let an AI
agent (Claude Code) do it with you: [docs/AGENT-INSTALL-PROMPT.md](docs/AGENT-INSTALL-PROMPT.md) has a prompt to paste.

> **At your own risk.** This writes one folder to the head unit's flash. Nothing else is changed, no coding, no ECU writes, and
> removing the folder undoes it. It has been tested on **one car only** (below). Read the whole guide before you start.

## Will it work on my car?

| | Tested | Notes |
|---|---|---|
| Head unit | NBT Evo (iDrive 6), firmware `NBTevo_X20511I`, split node (SSH on `hu-jacinto`, HMI on `hu-omap`) | other NBT Evo firmware: the agent checks the framework symbols before installing (step 3) |
| Cluster / HUD | 6WB cluster with the **factory HUD** | without a factory HUD there is nothing to draw on |
| Car | F34 340i (B58), 8-speed automatic | the gear comes from the car's data service, values seen on this car |
| Camera list | Ukraine (Patrol Police list, 426 cameras) | other countries: build your own list (step 7), e.g. the United Kingdom from OpenStreetMap |

Don't install it next to another HUD app (HUD Gauges / `HudTest`, Wayglass): two apps drawing on the HUD freeze it.
ExtendedHUD refuses to start when `HudTest` is installed.

## What you need

- A laptop with an **ENET cable** (OBD to Ethernet) and Python 3.
- **SSH unlocked** on the head unit: the Bavarian SSH Client (https://ssh.bavarian-aa.uk/) → Enable SSH over ENET. It lasts until
  the unit restarts. Use the root password that tool gives you. Never paste it into a public place.
- The package `extended-hud.zip` from this repository's Releases page, or built from source ([docs/BUILD.md](docs/BUILD.md)).
- Parked, ignition on, engine off. For long sessions keep the battery charged.

## Steps

### 1. Connect

Plug in the ENET cable, switch the ignition on, unlock SSH, then find the unit:

```sh
ping -b <interface> 169.254.255.255; arp -an     # the head unit answers on 169.254.x.x
ssh -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa \
    -o KexAlgorithms=+diffie-hellman-group14-sha1 root@<HEAD_IP> 'uname -a; ls /net'
```

The unit only offers old `ssh-rsa` keys, so the options above are needed. If `ls /net` lists `hu-omap`, it is a **split node**
(like the tested car): the HMI files live on `hu-omap`, reached as `/net/hu-omap/...`. Otherwise it is a single node.

| | App folder (as seen over SSH) | Frameworks |
|---|---|---|
| split node | `/net/hu-omap/fs/sda1/opt/hmi/data/ID5/Plugins/ExtendedHUD` | `/net/hu-omap/fs/sda0/repository/istep/opt/hmi/ID5/lib/` |
| single node | `/var/opt/hmi/data/ID5/Plugins/ExtendedHUD` | `/opt/hmi/ID5/lib/` (not seen on a car yet) |

### 2. Unpack and verify the package

```sh
mkdir extendedhud && cd extendedhud && unzip ../extended-hud.zip && shasum -a 256 -c SHA256SUMS
```

### 3. Check that the library fits your firmware (read-only)

Copy the two framework libraries from the car (they stay on your laptop: don't share them) and check every symbol:

```sh
ssh root@<HEAD_IP> 'cat <frameworks>/libAppFramework.A.so'  > libAppFramework.A.so
ssh root@<HEAD_IP> 'cat <frameworks>/libCoreFramework.A.so' > libCoreFramework.A.so
python3 abi_check.py ExtendedHUD/libExtendedHUD.so libAppFramework.A.so libCoreFramework.A.so   # in the zip; also tools/abi_check.py
```

`51 bmw symbols ok, pluginAppInfo=1` means it fits. If symbols are **MISSING**, stop: rebuild from source against your own frameworks
([docs/BUILD.md](docs/BUILD.md)) or open an issue with the output.

### 4. Look before writing

```sh
ssh root@<HEAD_IP> 'ls -la <Plugins folder>'          # Plugins = the app folder without /ExtendedHUD
```

Save the output. If `HudTest` is there, remove that app first (or don't install). If the `Plugins` folder doesn't exist, it is
created in step 5. Note that, because removing ExtendedHUD later means removing `Plugins` too.

### 5. Copy the app folder

```sh
D=<app folder>
ssh root@<HEAD_IP> "mkdir -p $D"
for f in ExtendedHUD/*; do ssh root@<HEAD_IP> "cat > $D/$(basename $f)" < "$f"; done
```

Make the two programs executable. On a split node: `ssh root@<HEAD_IP> 'on -f hu-omap chmod 755 /var/opt/hmi/data/ID5/Plugins/ExtendedHUD/hud-sportbar-qnx /var/opt/hmi/data/ID5/Plugins/ExtendedHUD/shift-deck-qnx'`.
If the node has no `chmod`, copy `/bin/cat` over the file name first (`cp /bin/cat X`), then `cat >` the real contents into it: it keeps the mode.

Read every file back and compare:

```sh
for f in ExtendedHUD/*; do [ "$(ssh root@<HEAD_IP> "cat $D/$(basename $f)" | shasum -a 256 | cut -d' ' -f1)" = \
  "$(shasum -a 256 < "$f" | cut -d' ' -f1)" ] && echo "ok $f" || echo "BAD $f"; done
```

### 6. Restart iDrive and switch it on

Hold the volume knob until the screen goes dark (about 30 s). The SSH connection drops, which is normal. Then open
**My Vehicle → ExtendedHUD**:

| Row | Does |
|---|---|
| status line | Off / Starting / Running |
| Sport Bar | the rev bar with the engine running in D/S/M, in Sport and Sport+ |
| Always show Sport Bar | the bar in every driving mode |
| Shift point | the gear starts blinking at 6,200–6,600 RPM |
| Display Speed Camera Warning | the camera icon, its limit and the distance, from the warning distance before the camera to 50 m past it, only for cameras that watch your direction |
| Orange camera warning | the camera and its limit in orange instead of white |
| Warning distance | 300, 500 (default) or 800 m |
| App information | the version and release date, and the camera list's country and **data date** |

The camera warning needs the car's GPS (shipped in `cds.txt`) and the engine running.

### 7. Keep the camera list current

The list is not tied to app releases. Check its age in **App information**. To refresh it, or to build one for another country,
ask your agent to follow the skill [`.claude/skills/update-speedcam-db`](.claude/skills/update-speedcam-db/SKILL.md), or run:

```sh
python3 tools/cameras_build.py --country Ukraine --fetch --fetch-osm /tmp/ua-osm.json --out cameras.txt
python3 tools/cameras_build.py --country UnitedKingdom --fetch-osm /tmp/gb-osm.json --out cameras.txt
```

Then copy `cameras.txt` to the app folder (upload as `cameras.txt.new`, read it back, `mv` it over the old one) and untick and tick
Display Speed Camera Warning. Countries and their sources: `tools/cameras_sources.json`.

## Update the app

Upload each changed file under a new name, read it back, then `mv` it over the old one: the running HMI and worker keep the old
file until the next restart. Never `cat >` over the installed `libExtendedHUD.so` or a running program. Then restart iDrive.
Your settings (`settings.txt`) stay.

## Remove it

```sh
ssh root@<HEAD_IP> 'rm -rf <app folder>'     # and the Plugins folder too, if it didn't exist before step 5
```

Then restart iDrive. If the HMI ever keeps restarting, SSH stays reachable: remove the folder the same way.

## Troubleshooting

| Symptom | Check |
|---|---|
| no ExtendedHUD in My Vehicle | step 3 (symbols); files all `ok` in step 5; the iDrive restart. Some cars may need the head unit's apps coding (A4A / remote apps): not needed on the tested car |
| status "Off: remove HudTest first" | another HUD app is installed |
| no bar while revving | P, N or R, or Comfort without "Always"; the log: `ssh root@<HEAD_IP> 'on -f hu-omap cat /tmp/hud-sportbar.log'` (single node: `cat /tmp/hud-sportbar.log`) |
| no camera warning | engine off; "Display Speed Camera Warning" unticked; the camera watches the other direction; App information says "no list installed" |
| App information: "data date unknown" | an old or hand-made `cameras.txt`: rebuild it (step 7) |
