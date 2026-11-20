---
name: update-speedcam-db
description: Refresh or build ExtendedHUD's speed-camera database (cameras.txt) for a single country using tools/cameras_sources.json (e.g., Ukraine: Patrol Police table + OpenStreetMap directions; United Kingdom: OpenStreetMap), verify output quality, and optionally deploy cameras.txt to a target vehicle. Independent of app releases. Use when updating or rebuilding the camera database, adding a new country, or when iDrive App information shows an outdated data date.
---

# Update the Speed-Camera Database

The speed-camera database (`cameras.txt`) is decoupled from application code. Refreshing it requires no firmware rebuild or plugin release: `tools/cameras_build.py` generates a standalone `cameras.txt`, and **My Vehicle → ExtendedHUD → App information** reads its `# country` and `# date` header lines automatically.

**Fetch only the requested country.** Sources are organized per country in `tools/cameras_sources.json`. Never fetch all countries at once — nationwide OpenStreetMap queries can take several minutes and public Overpass endpoints enforce rate limits.

## 1. Identify Target Country and Baseline

- Determine the country key from the user's request or from the `# country` header of the currently installed `cameras.txt` (`native/plugin/cameras.txt` or the vehicle's copy). Ask if ambiguous. Valid keys match `tools/cameras_sources.json` (e.g., `Ukraine`, `UnitedKingdom`).
- Review the country's configuration entry: `list` (primary camera/limit source), `directions`, `other` (secondary sources and last checked status), `traffic` (`right`/`left`), and `limit_unit`.
- Save a baseline copy for diffing: `cp native/plugin/cameras.txt /tmp/cameras-old.txt` (or pull the current copy from the vehicle).

## 2. Fetch and Build (Read-Only, Network Only)

Use a fresh scratch directory for downloads. Never execute downloaded artifacts.

| List Kind | Command |
|---|---|
| `patrolpolice_ua` (Ukraine) | `python3 tools/cameras_build.py --country Ukraine --fetch --fetch-osm <SCRATCH>/ua-osm-<DATE>.json --out <SCRATCH>/cameras.txt` |
| `osm_country` (United Kingdom, other OSM-only countries) | `python3 tools/cameras_build.py --country UnitedKingdom --fetch-osm <SCRATCH>/gb-osm-<DATE>.json --out <SCRATCH>/cameras.txt` |

- `--fetch` stamps the current date. When rebuilding from a saved HTML snapshot, pass `--html page-YYYY-MM-DD.html` (date inferred from filename) or `--date YYYY-MM-DD`.
- Overpass rejects requests without a `User-Agent` (HTTP 406); the builder sets one and rotates across three endpoints. If all endpoints fail, retry later rather than retrying aggressively. For Ukraine, the builder automatically falls back to the OSM API.
- If an upstream HTML table layout changes and `parse_table` logs skipped rows or zero results, update the parser and add a unit test covering the new layout rather than hand-editing coordinates.

## 3. Check Secondary Sources

For the selected country, review entries under `other` in `tools/cameras_sources.json` whose status was previously unavailable or unchecked, and check for newly published official open-data portals:

- **Ukraine example:** The `data.gov.ua` National Police open dataset (`Cameras.csv`) includes a dedicated direction column; verify whether the endpoint is available and, if accessible, integrate it as a proper builder tier with unit tests rather than hand-patching `cameras.txt`.
- Record checked dates and updated statuses in `tools/cameras_sources.json`.

Any newly integrated source should be added as a `list` or `directions` handler in `tools/cameras_build.py` accompanied by unit tests in `tools/test_cameras_build.py`.

## 4. Validate Before Deployment

```bash
head -4 <SCRATCH>/cameras.txt                                 # Verify source attribution, # country, # date, and rule counts
python3 -B -m unittest discover -s tools -p 'test_*.py'       # Run builder unit tests
```

Compare against the baseline file and report a summary to the user:
- Total camera count, count with resolved direction (`dir >= 0`), and count without a speed limit (`limit 0`, which never alert).
- Added and removed entries (match cameras by `lat lon` rounded to 4 decimal places). A drop exceeding ~10% typically indicates an upstream fetch or parse issue — investigate before deploying.
- Spot-check 2–3 newly added coordinates on a map to confirm plausible road placement and speed limit.

If updating the repository's bundled `native/plugin/cameras.txt`, also run `make -C native test`, which parses the bundled file, verifies its country/date headers, and executes regression fixtures against the bundled dataset (so `native/plugin/cameras.txt` remains on the default bundled country). Country databases built for other regions should be deployed directly to the target vehicle (step 5) rather than overwriting `native/plugin/cameras.txt`. Commit repository changes only when requested.

## 5. Deploy to Vehicle (Optional, Explicit Approval Required)

Deploying replaces a single file (`cameras.txt`) inside the plugin directory. Confirm with the user first. Refer to `INSTALL.md` for SSH connection options.

| Node Layout | Plugin Directory (as seen via SSH) |
|---|---|
| Split-node (`hu-jacinto` SSH / `hu-omap` HMI) | `/net/hu-omap/fs/sda1/opt/hmi/data/ID5/Plugins/ExtendedHUD` |
| Single-node | `/var/opt/hmi/data/ID5/Plugins/ExtendedHUD` |

```bash
D=<app folder>
ssh root@<HEAD_IP> "cat $D/cameras.txt" > <SCRATCH>/cameras-before.txt               # Local backup
ssh root@<HEAD_IP> "cat > $D/cameras.txt.new" < <SCRATCH>/cameras.txt
[ "$(ssh root@<HEAD_IP> "cat $D/cameras.txt.new" | shasum -a 256)" = "$(shasum -a 256 < <SCRATCH>/cameras.txt)" ] \
  && ssh root@<HEAD_IP> "mv $D/cameras.txt.new $D/cameras.txt"                       # Atomic swap after SHA-256 match
```

The worker loads `cameras.txt` at startup. Have the user uncheck and re-check **Display Speed Camera Warning** (or restart iDrive), then inspect **App information** to confirm the updated country and date. To roll back, restore `<SCRATCH>/cameras-before.txt` using the same readback-and-swap sequence.

## Adding a New Country

1. Add a key in `tools/cameras_sources.json` specifying `name` (displayed in iDrive), `iso` (ISO 3166-1 alpha-2), `traffic` (`right`/`left`), `limit_unit` (`km/h` or `mph`), `list`, `directions`, and `other`.
2. For OpenStreetMap-based datasets, `"kind": "osm_country"` works out of the box. For an official national registry, add a parser kind to `tools/cameras_build.py` returning `{lat, lon, limit, place, region}` rows along with a fixture test.
3. Build, validate (step 4), and report how many cameras lack speed limits or direction tags.

## Format and Data Notes

- **Line format:** `lat lon limit dir`, where `dir` is the monitored travel bearing in degrees clockwise from North, or `-1` for bidirectional monitoring. The worker alerts from the configured **Warning distance** (`300`/`500`/`800` m) before the camera to 50 m past it when the vehicle's heading is within 50° of `dir`. Capacity is up to 16,384 entries.
- **Bidirectional & ambiguous entries:** Dual-direction radar complexes (such as KASKAD, Expert-FS, HOAG-50) and entries with imprecise coordinates or unresolvable geometry remain `-1` so they alert safely in both directions.
- **OSM `direction=` caveat:** Numeric `direction=` degrees in OpenStreetMap frequently represent lens azimuth rather than direction of travel, so the builder ignores numeric degrees and relies on `forward`/`backward`/`both` tags and road geometry.
- **Licensing:** OpenStreetMap data is licensed under ODbL; preserve the source attribution line in the `cameras.txt` header.
