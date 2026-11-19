#!/usr/bin/env python3
"""Speed-camera list for ExtendedHUD (story speed-cameras): the Patrol Police table of fixed cameras -> cameras.txt.

    python3 tools/cameras_build.py --html kaf.html [--osm ways.json] [--out native/plugin/cameras.txt]
    python3 tools/cameras_build.py --fetch ...      # downloads the table first (patrolpolice.gov.ua)

cameras.txt: "lat lon limit dir" per line (hud_cam.h). dir is the direction of travel the camera watches, degrees from
north, or -1 when unknown (the HUD then shows it both ways). The police table has no direction column, so it comes from
OpenStreetMap roads near each camera (--osm, an Overpass "out geom" dump):
  - the nearest road is one-way (a carriageway of a divided road): the camera watches that way;
  - a two-way road: right-hand traffic, so a camera watches the lane beside it: the side of the road it stands on decides.
    For a pair (two cameras within 150 m) the one farther right of the road's line watches the road's way, the other the
    opposite way; a single camera needs to stand at least SIDE_M off the centre line, else it stays -1.
"""
import argparse
import html
import json
import math
import re
import sys
import urllib.request

URL = "https://patrolpolice.gov.ua/automatic_fixation/informatsiya-pro-vstanovleni-kaf/"
PAIR_M = 150.0
NEAR_M = 30.0
SIDE_M = 2.5


def parse_table(text):
    """The police page's one table -> [{'lat','lon','limit','place','region'}]."""
    a, b = text.find("<table"), text.find("</table>")
    if a < 0 or b < 0:
        raise SystemExit("no table in the page")
    rows = []
    for tr in re.findall(r"<tr.*?</tr>", text[a:b], re.S):
        cells = [re.sub(r"\s+", " ", html.unescape(re.sub(r"<[^>]+>", "", c))).strip()
                 for c in re.findall(r"<t[dh][^>]*>(.*?)</t[dh]>", tr, re.S)]
        if len(cells) != 7 or not cells[0].isdigit():
            continue
        gps = cells[6]
        if gps.count(",") == 3:   # "50, 769657767, 25, 357070027": commas as decimal points (3 rows, 2026-11-19)
            gps = re.sub(r"(\d+), (\d+), (\d+), (\d+)", r"\1.\2, \3.\4", gps)
        m = re.findall(r"-?\d+\.\d+", gps)
        lim = re.findall(r"\d+", cells[5])
        if len(m) != 2:
            print("skipped (no GPS):", cells, file=sys.stderr)
            continue
        lat, lon = float(m[0]), float(m[1])
        if not (44 < lat < 53 and 22 < lon < 41):
            print("skipped (outside Ukraine):", cells, file=sys.stderr)
            continue
        rows.append({"lat": lat, "lon": lon, "limit": int(lim[0]) if lim else 0, "place": cells[4], "region": cells[2]})
    return rows


def xy(lat0, lon0, lat, lon):
    """Metres east, north of (lat0, lon0); flat earth, as hud_geo_offset."""
    return ((lon - lon0) * 111320.0 * math.cos(math.radians(lat0)), (lat - lat0) * 111320.0)


def bearing(dx, dy):
    return (math.degrees(math.atan2(dx, dy)) + 360.0) % 360.0


def nearest_segment(cam, ways):
    """(distance, bearing of the way's node order there, signed offset right of that bearing, oneway) or None."""
    best = None
    for w in ways:
        g = w.get("geometry") or []
        tags = w.get("tags", {})
        ow = tags.get("oneway")
        oneway = 1 if ow in ("yes", "true", "1") or tags.get("highway") == "motorway" or tags.get("junction") == "roundabout" \
            else -1 if ow == "-1" else 0
        for p, q in zip(g, g[1:]):
            ax, ay = xy(cam["lat"], cam["lon"], p["lat"], p["lon"])
            bx, by = xy(cam["lat"], cam["lon"], q["lat"], q["lon"])
            sx, sy = bx - ax, by - ay
            L2 = sx * sx + sy * sy
            if L2 < 1e-6:
                continue
            t = max(0.0, min(1.0, -(ax * sx + ay * sy) / L2))
            cx, cy = ax + t * sx, ay + t * sy   # the nearest point; the camera is at (0, 0)
            d = math.hypot(cx, cy)
            # right of the direction (sx, sy) is (sy, -sx); the camera's offset from the road is (-cx, -cy)
            side = (-cx * sy + cy * sx) / math.sqrt(L2)
            if best is None or d < best[0]:
                best = (d, bearing(sx, sy), side, oneway)
    return best


def directions(rows, ways):
    out = [-1] * len(rows)
    if not ways:
        return out, {"no osm": len(rows)}
    stats = {}
    near = [nearest_segment(r, ways) for r in rows]

    def note(k):
        stats[k] = stats.get(k, 0) + 1

    for i, r in enumerate(rows):
        s = near[i]
        if s is None or s[0] > NEAR_M:
            note("no road within %d m" % NEAR_M)
            continue
        d, br, side, oneway = s
        if oneway:
            out[i] = round(br if oneway > 0 else (br + 180) % 360) % 360
            note("one-way road")
            continue
        mates = [j for j, q in enumerate(rows) if j != i and math.hypot(*xy(r["lat"], r["lon"], q["lat"], q["lon"])) < PAIR_M
                 and near[j] and near[j][0] <= NEAR_M and abs(((near[j][1] - br + 180) % 360) - 180) < 30]
        if len(mates) == 1:   # a pair on one road: farther right watches the way's direction
            other = near[mates[0]]
            if abs(side - other[2]) >= 2 * SIDE_M:
                out[i] = round(br if side > other[2] else (br + 180) % 360) % 360
                note("pair, by side")
                continue
        if abs(side) >= SIDE_M:
            out[i] = round(br if side > 0 else (br + 180) % 360) % 360
            note("single, by side")
        else:
            note("on the centre line: unknown")
    return out, stats


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--html", help="the saved police page")
    ap.add_argument("--fetch", action="store_true", help="download the police page (saved next to --out as .html)")
    ap.add_argument("--osm", help="Overpass JSON of the roads near the cameras (out geom tags)")
    ap.add_argument("--out", default="native/plugin/cameras.txt")
    a = ap.parse_args()
    if a.fetch:
        req = urllib.request.Request(URL, headers={"User-Agent": "Mozilla/5.0 (ExtendedHUD camera list)"})
        text = urllib.request.urlopen(req, timeout=60).read().decode("utf-8")
    elif a.html:
        text = open(a.html, encoding="utf-8").read()
    else:
        ap.error("--html or --fetch")
    rows = parse_table(text)
    ways = json.load(open(a.osm))["elements"] if a.osm else []
    dirs, stats = directions(rows, ways)
    with open(a.out, "w") as f:
        f.write("# ExtendedHUD speed cameras: lat lon limit dir (hud_cam.h). Source: Patrol Police, %s\n" % URL)
        f.write("# %d fixed cameras; dir from OpenStreetMap: %s\n" % (len(rows), ", ".join("%s %d" % kv for kv in sorted(stats.items()))))
        for r, d in zip(rows, dirs):
            f.write("%.6f %.6f %d %d\n" % (r["lat"], r["lon"], r["limit"], d))
    known = sum(1 for d in dirs if d >= 0)
    print("%d cameras -> %s; direction known for %d (%s)" % (len(rows), a.out, known, stats))


if __name__ == "__main__":
    main()
