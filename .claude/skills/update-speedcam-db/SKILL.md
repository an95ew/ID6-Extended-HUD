---
name: update-speedcam-db
description: Refresh ExtendedHUD's speed-camera list (cameras.txt) for one country from the sources in tools/cameras_sources.json (Ukraine: Patrol Police table + OpenStreetMap directions; United Kingdom: OpenStreetMap), verify it, and optionally copy it to the car. Independent of app releases, so anyone can keep their cameras current with an agent. Use when asked to update, refresh or rebuild the camera database, to add a country, or when the iDrive App information page shows an old "data from" date.
---

# Update the speed-camera database

The camera list is data, not code. Updating it needs no release and no rebuild: `tools/cameras_build.py` writes a new
`cameras.txt`, and the car only needs that one file. The iDrive page **My Vehicle → ExtendedHUD → App information** then shows
"Speed cameras: <country>, <count>, data from <date>", read from the file's `# country` and `# date` lines.

**Fetch only the country asked for.** Sources are grouped per country in `tools/cameras_sources.json`. Never download every country
"to be safe": some fetches are big (the UK OSM query takes ~5 minutes), and public servers have usage policies.

## 1. Which country, and what's on the car now

- The country: the user's words, or the `# country` line of the list they use now (`native/plugin/cameras.txt` in the repo, or
  the copy on the car, step 5). If it's still unclear, ask. Keys are the JSON keys: `Ukraine`, `UnitedKingdom`, ...
- Read that country's entry: `list` (where the cameras and limits come from), `directions`, `other` (more sources and their last
  known status), `traffic` (right/left) and `limit_unit`.
- Keep the old file for the comparison: `cp native/plugin/cameras.txt /tmp/cameras-old.txt` (or the car's copy).

## 2. Fetch and build (read-only, internet only)

Work in a fresh scratch folder for downloads. Don't run anything you downloaded.

| list kind | Command |
|---|---|
| `patrolpolice_ua` (Ukraine) | `python3 tools/cameras_build.py --country Ukraine --fetch --fetch-osm SCRATCH/ua-osm-DATE.json --out SCRATCH/cameras.txt` |
| `osm_country` (UK, any OSM-only country) | `python3 tools/cameras_build.py --country UnitedKingdom --fetch-osm SCRATCH/gb-osm-DATE.json --out SCRATCH/cameras.txt` |

- `--fetch` sets the date to today. To rebuild from a saved page: `--html page-YYYY-MM-DD.html` (the date comes from the file name)
  or `--date YYYY-MM-DD`.
- Overpass refuses requests without a User-Agent (HTTP 406). The builder sends one, and it tries three endpoints. If all fail, retry
  later rather than hammering them. For Ukraine the builder falls back to the OSM API automatically.
- The police site is a WordPress page. If its table layout changes, `parse_table` prints "skipped" lines or finds 0 rows. Fix the
  parser and add a test with the new layout. Never hand-edit coordinates.

## 3. Try the other sources ("maybe more")

For the chosen country, go through `other` in `cameras_sources.json` and retry each one whose status says it was down or unchecked.
Look for new official sources too (police open data, road authority, national data portal):

- Ukraine: `data.gov.ua` "Місцезнаходження камер автоматичної фіксації" (National Police `Cameras.csv`, **has a direction column**,
  down since 2026-11-19). If it answers, use its direction for the cameras still shown both ways (`-1`). Add it as a builder tier
  with a test; don't patch `cameras.txt` by hand.
- Record what you found: update that source's `status` in `cameras_sources.json` with the date, and add any new source you used.

A new source becomes a `list` or `directions` kind in `cameras_build.py`, with a unit test in `tools/test_cameras_build.py`.

## 4. Check the result before anyone drives with it

```bash
head -4 SCRATCH/cameras.txt                                   # source, "# country", "# date", counts per direction rule
python3 -B -m unittest discover -s tools -p 'test_*.py'       # the builder's tests
```

Compare with the old list and report the numbers to the user:
- Total cameras, cameras with a direction (`dir >= 0`), cameras without a limit (`limit 0` never alert).
- Added and removed cameras: same `lat lon` rounded to 4 decimals = the same camera. A big drop (more than ~10 %) usually means a
  fetch or parse problem, not that cameras were removed. Stop and find out why before shipping it.
- Spot-check 2 or 3 new cameras on a map (the coordinates should be on a road, the limit plausible).

For the repo copy (`native/plugin/cameras.txt`, the list shipped in the package), also run `make -C native test`. It parses the
shipped file, checks its country and date, and replays real Kyiv cameras, so **the repo copy stays Ukraine**. Another country's list
goes only to that user's car (step 5), not into `native/plugin/`. Commit only when the user asks, with a message that names the
country and the date.

## 5. Put it on the car (optional, needs the user's yes)

This is the only write, one file in the app folder. Ask first. Connection: the user's SSH setup (`INSTALL.md` § SSH). The paths:

| Unit | App folder as seen from the SSH node |
|---|---|
| split node (SSH on hu-jacinto, HMI on hu-omap) | `/net/hu-omap/fs/sda1/opt/hmi/data/ID5/Plugins/ExtendedHUD` |
| single node | `/var/opt/hmi/data/ID5/Plugins/ExtendedHUD` |

```bash
D=<app folder>
ssh root@HEAD "cat $D/cameras.txt" > SCRATCH/cameras-car-before.txt                 # the backup, kept on the laptop
ssh root@HEAD "cat > $D/cameras.txt.new" < SCRATCH/cameras.txt
[ "$(ssh root@HEAD "cat $D/cameras.txt.new" | shasum -a 256)" = "$(shasum -a 256 < SCRATCH/cameras.txt)" ] \
  && ssh root@HEAD "mv $D/cameras.txt.new $D/cameras.txt"                            # swap only after the readback matches
```

The worker reads the list when it starts. Tell the user to untick and tick **Display Speed Camera Warning** (or restart iDrive), then
open **App information**: it should show the new country and date. To roll back, copy `cameras-car-before.txt` back the same way.

## Add a country

1. Add a key to `tools/cameras_sources.json` with `name` (shown in the menu), `iso` (ISO 3166-1 alpha-2), `traffic`
   (`right`/`left`: which side of a two-way road a camera serves), `limit_unit` (as on the road signs), `list`, `directions`, `other`.
2. If OpenStreetMap is the best open source there, `"kind": "osm_country"` works as it is. For an official list, write a new kind
   in `cameras_build.py` (a parser returning `{lat, lon, limit, place, region}` rows) and a test with a small sample of the real format.
3. Build, check (step 4), and tell the user how many cameras have no limit or no direction. Those are shown both ways or never.

## Facts worth knowing

- `cameras.txt` line: `lat lon limit dir`. `dir` = direction of travel the camera watches, degrees from north, `-1` = both ways.
  The worker alerts from the menu's Warning distance (300/500/800 m) before the camera to 50 m after it, only when the car's
  heading is within 50° of `dir`. It holds up to 16,384 cameras and reads a file of any size.
- Ukraine (2026-11-20): 426 cameras, 378 with a direction. 40 are radar complexes (KASKAD, Expert-FS, HOAG-50) whose OSM notes say
  they control both directions: `-1` is right for them. 8 more are unknown: three police coordinates lie 2.6–8.6 km from their road,
  one is at a crossroads, four sit on a road's centre line. Details: `.claude/stories/speed-cameras/` (local notes, if present).
- OSM numeric `direction=` degrees are often the lens azimuth, not the direction of travel (7 of 9 were opposite on one-way roads), so
  the builder ignores them. `forward`/`backward` are reliable.
- OpenStreetMap data is ODbL: keep the source line in the file header.
