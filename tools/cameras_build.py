#!/usr/bin/env python3
"""Speed-camera list for ExtendedHUD (story speed-cameras): the Patrol Police table of fixed cameras -> cameras.txt.

    python3 tools/cameras_build.py --html kaf.html [--osm ways.json] [--out native/plugin/cameras.txt]
    python3 tools/cameras_build.py --fetch [--fetch-osm ways.json] ...

cameras.txt: "lat lon limit dir" per line (hud_cam.h). dir is the direction of travel the camera watches, degrees from
north, or -1 when unknown or bidirectional (the HUD then shows it both ways). The police table has no direction column,
so directions come from OpenStreetMap elements near each camera (--osm: ways + highway=speed_camera nodes):
  1. Explicit OSM highway=speed_camera nodes (1-to-1 within 25 m) tagged direction=forward|backward|both (113/113 forward
     on one-way roads; both -> -1). Numeric degrees are skipped (7/9 opposite on one-way roads: lens azimuth vs travel).
  2. One-way carriageway (divided road) within 30 m: the camera watches that way.
  3. Two-way road within 30 m: right-hand traffic, so pairs (<150 m, anti-parallel safe) split by side and singles
     at least SIDE_M off the centre line take their side; otherwise -1.
"""
import argparse
import html
import json
import math
import os
import re
import sys
import time
import urllib.parse
import urllib.request

URL = "https://patrolpolice.gov.ua/automatic_fixation/informatsiya-pro-vstanovleni-kaf/"
PAIR_M = 150.0
NEAR_M = 30.0
SIDE_M = 2.5
OSM_CAM_M = 25.0

MAJOR_HIGHWAYS = {"motorway", "trunk", "primary", "secondary", "tertiary"}
DRIVABLE_HIGHWAYS = MAJOR_HIGHWAYS | {
    "unclassified", "residential", "living_street",
    "motorway_link", "trunk_link", "primary_link", "secondary_link", "tertiary_link",
}
OVERPASS_ENDPOINTS = (
    "https://overpass-api.de/api/interpreter",
    "https://overpass.kumi.systems/api/interpreter",
    "https://overpass.openstreetmap.fr/api/interpreter",
)
UA = "ExtendedHUD-CameraBuild/1.0"


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
    """(distance, bearing of the way's node order there, signed offset right of that bearing, oneway) or None.
    Ignores non-drivable ways (footway/service/etc.) and prefers major roads over nearby minor side streets."""
    best = None
    best_major = None
    for w in ways:
        if w.get("type", "way") != "way":
            continue
        g = w.get("geometry") or []
        tags = w.get("tags", {})
        hw = tags.get("highway", "")
        if hw and hw not in DRIVABLE_HIGHWAYS:
            continue
        ow = tags.get("oneway")
        oneway = 1 if ow in ("yes", "true", "1") or hw == "motorway" or tags.get("junction") == "roundabout" \
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
            cand = (d, bearing(sx, sy), side, oneway, hw)
            if best is None or d < best[0]:
                best = cand
            if hw in MAJOR_HIGHWAYS and (best_major is None or d < best_major[0]):
                best_major = cand
    if best_major and best and best[4] not in MAJOR_HIGHWAYS and best_major[0] <= 20.0 and best_major[0] - best[0] <= 8.0:
        return best_major[:4]
    return best[:4] if best else None


def _node_way_bearing(nd, ways):
    """Local bearing of the drivable OSM way containing node nd (or nearest within 20 m)."""
    nlat, nlon = nd["lat"], nd["lon"]
    for w in ways:
        tags = w.get("tags", {})
        hw = tags.get("highway", "")
        if hw and hw not in DRIVABLE_HIGHWAYS:
            continue
        g = w.get("geometry") or []
        for k, pt in enumerate(g):
            if abs(pt["lat"] - nlat) < 1e-6 and abs(pt["lon"] - nlon) < 1e-6:
                p0 = g[k - 1] if k > 0 else g[k]
                p1 = g[k + 1] if k + 1 < len(g) else g[k]
                dx, dy = xy(nlat, nlon, p1["lat"], p1["lon"])
                dx0, dy0 = xy(nlat, nlon, p0["lat"], p0["lon"])
                if math.hypot(dx - dx0, dy - dy0) > 1e-3:
                    return bearing(dx - dx0, dy - dy0)
    s = nearest_segment({"lat": nlat, "lon": nlon}, ways)
    return s[1] if s and s[0] <= 20.0 else None


def directions(rows, elements):
    out = [-1] * len(rows)
    if not elements:
        return out, {"no osm": len(rows)}
    ways = [e for e in elements if e.get("type", "way") == "way"]
    cam_nodes = [e for e in elements if e.get("type") == "node" and e.get("tags", {}).get("highway") == "speed_camera"]
    reasons = [None] * len(rows)
    near = [nearest_segment(r, ways) for r in rows]

    # Tier 1: 1-to-1 greedy matching to OSM highway=speed_camera nodes with explicit forward/backward/both (<= OSM_CAM_M).
    cand_pairs = []
    for i, r in enumerate(rows):
        for j, nd in enumerate(cam_nodes):
            dtag = nd.get("tags", {}).get("direction", "").strip().lower()
            if dtag not in ("forward", "backward", "both"):
                continue
            d = math.hypot(*xy(r["lat"], r["lon"], nd["lat"], nd["lon"]))
            if d <= OSM_CAM_M:
                cand_pairs.append((d, i, j))
    cand_pairs.sort()
    row_to_nd = {}
    used_nd = set()
    for d, i, j in cand_pairs:
        if i in row_to_nd or j in used_nd:
            continue
        row_to_nd[i] = cam_nodes[j]
        used_nd.add(j)

    for i, r in enumerate(rows):
        if i in row_to_nd:
            nd = row_to_nd[i]
            dtag = nd["tags"]["direction"].strip().lower()
            if dtag == "both":
                reasons[i] = "osm speed_camera: both ways"
                continue
            br = _node_way_bearing(nd, ways)
            if br is not None:
                out[i] = round(br if dtag == "forward" else (br + 180) % 360) % 360
                reasons[i] = "osm speed_camera (%s)" % dtag
                continue
        s = near[i]
        if s is None or s[0] > NEAR_M:
            reasons[i] = "no road within %d m" % NEAR_M
            continue
        d, br, side, oneway = s
        if oneway:
            out[i] = round(br if oneway > 0 else (br + 180) % 360) % 360
            reasons[i] = "one-way road"
            continue
        # Two-way pair (< PAIR_M): handle anti-parallel OSM segments by flipping the mate's side sign.
        mates = []
        for j, q in enumerate(rows):
            if j == i or not near[j] or near[j][0] > NEAR_M or near[j][3]:
                continue
            if math.hypot(*xy(r["lat"], r["lon"], q["lat"], q["lon"])) >= PAIR_M:
                continue
            diff = abs(((near[j][1] - br + 180) % 360) - 180)
            if diff < 30:
                mates.append((j, near[j][2]))
            elif diff > 150:
                mates.append((j, -near[j][2]))
        if len(mates) == 1:
            other_side = mates[0][1]
            if abs(side - other_side) >= 2 * SIDE_M:
                out[i] = round(br if side > other_side else (br + 180) % 360) % 360
                reasons[i] = "pair, by side"
                continue
        if abs(side) >= SIDE_M:
            out[i] = round(br if side > 0 else (br + 180) % 360) % 360
            reasons[i] = "single, by side"
        else:
            reasons[i] = "on the centre line: unknown"

    # Median duplicate coordinates (< 2 m apart, same resolved direction): split the second to the opposite direction.
    for i in range(len(rows)):
        for j in range(i + 1, len(rows)):
            if out[i] >= 0 and out[i] == out[j] and math.hypot(*xy(rows[i]["lat"], rows[i]["lon"], rows[j]["lat"], rows[j]["lon"])) < 2.0:
                out[j] = (out[i] + 180) % 360
                reasons[j] = "median pair split"
    # Pair completion: an unknown camera (not OSM "both") whose only neighbour within PAIR_M is directed watches the
    # opposite way (2026-11-20: all 54 directed pairs are opposite; e.g. Pivdennyi Bridge "на правий берег" vs row 10).
    for i, r in enumerate(rows):
        if out[i] >= 0 or reasons[i] == "osm speed_camera: both ways":
            continue
        mates = [j for j, q in enumerate(rows) if j != i and math.hypot(*xy(r["lat"], r["lon"], q["lat"], q["lon"])) < PAIR_M]
        if len(mates) == 1 and out[mates[0]] >= 0 and reasons[mates[0]] != "pair completion":
            out[i] = (out[mates[0]] + 180) % 360
            reasons[i] = "pair completion"
    stats = {}
    for k in reasons:
        stats[k] = stats.get(k, 0) + 1
    return out, stats


def fetch_osm(rows, out_path):
    """Fetch nearby drivable OSM ways + speed_camera nodes via Overpass (falling back to OSM API v0.6 /map.json)."""
    hw_re = "^(" + "|".join(sorted(DRIVABLE_HIGHWAYS)) + ")$"

    def req_json(url, data=None, timeout=30):
        r = urllib.request.Request(url, data=data, headers={"User-Agent": UA, "Accept": "application/json"})
        with urllib.request.urlopen(r, timeout=timeout) as resp:
            return json.loads(resp.read().decode("utf-8"))

    # Probe Overpass endpoints first
    ep = None
    probe = '[out:json][timeout:15];way(around:45,%.6f,%.6f)["highway"~"%s"];out geom tags;' % (rows[0]["lat"], rows[0]["lon"], hw_re)
    for url in OVERPASS_ENDPOINTS:
        try:
            j = req_json(url, data=urllib.parse.urlencode({"data": probe}).encode(), timeout=20)
            if j.get("elements"):
                ep = url
                break
        except Exception:
            pass

    elems = {}
    if ep:
        for i in range(0, len(rows), 35):
            batch = rows[i:i + 35]
            lines = ["[out:json][timeout:60];("]
            for r in batch:
                lines.append('way(around:45,%.6f,%.6f)["highway"~"%s"];' % (r["lat"], r["lon"], hw_re))
                lines.append('node(around:60,%.6f,%.6f)["highway"="speed_camera"];' % (r["lat"], r["lon"]))
            lines.append(");out geom tags;")
            j = req_json(ep, data=urllib.parse.urlencode({"data": "\n".join(lines)}).encode(), timeout=75)
            for e in j.get("elements", []):
                elems[(e["type"], e["id"])] = e
            time.sleep(1.5)
        source = ep
    else:
        # Cluster cameras within 120 m and query OSM API v0.6 /map.json
        used = [False] * len(rows)
        clusters = []
        for i, r in enumerate(rows):
            if used[i]:
                continue
            cl = [(r["lat"], r["lon"])]
            used[i] = True
            for j in range(i + 1, len(rows)):
                if not used[j] and math.hypot(*xy(r["lat"], r["lon"], rows[j]["lat"], rows[j]["lon"])) < 120.0:
                    cl.append((rows[j]["lat"], rows[j]["lon"]))
                    used[j] = True
            clusters.append(cl)
        for cl in clusters:
            lats, lons = [p[0] for p in cl], [p[1] for p in cl]
            dlat = 55.0 / 111320.0
            dlon = 55.0 / (111320.0 * math.cos(math.radians(lats[0])))
            bbox = "%.6f,%.6f,%.6f,%.6f" % (min(lons) - dlon, min(lats) - dlat, max(lons) + dlon, max(lats) + dlat)
            j = req_json("https://api.openstreetmap.org/api/0.6/map.json?bbox=" + bbox, timeout=25)
            nds = {e["id"]: (e["lat"], e["lon"]) for e in j.get("elements", []) if e["type"] == "node"}
            for e in j.get("elements", []):
                tags = e.get("tags", {})
                if e["type"] == "way" and tags.get("highway") in DRIVABLE_HIGHWAYS:
                    geom = [{"lat": nds[nid][0], "lon": nds[nid][1]} for nid in e.get("nodes", []) if nid in nds]
                    if len(geom) >= 2:
                        elems[("way", e["id"])] = {"type": "way", "id": e["id"], "tags": tags, "geometry": geom}
                elif e["type"] == "node" and tags.get("highway") == "speed_camera":
                    elems[("node", e["id"])] = {"type": "node", "id": e["id"], "lat": e["lat"], "lon": e["lon"], "tags": tags}
            time.sleep(0.25)
        source = "osm-api-0.6"

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    payload = {"source": source, "elements": list(elems.values())}
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(payload, f)
    return payload["elements"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--html", help="the saved police page")
    ap.add_argument("--fetch", action="store_true", help="download the police page")
    ap.add_argument("--osm", help="OSM JSON of roads + speed_camera nodes (Overpass out geom or --fetch-osm)")
    ap.add_argument("--fetch-osm", metavar="PATH", help="download OSM roads + speed_camera nodes and cache to PATH")
    ap.add_argument("--out", default="native/plugin/cameras.txt")
    a = ap.parse_args()
    if a.fetch:
        req = urllib.request.Request(URL, headers={"User-Agent": UA})
        text = urllib.request.urlopen(req, timeout=60).read().decode("utf-8")
    elif a.html:
        text = open(a.html, encoding="utf-8").read()
    else:
        ap.error("--html or --fetch")
    rows = parse_table(text)
    if a.fetch_osm:
        elements = fetch_osm(rows, a.fetch_osm)
    elif a.osm:
        elements = json.load(open(a.osm, encoding="utf-8"))["elements"]
    else:
        elements = []
    dirs, stats = directions(rows, elements)
    with open(a.out, "w", encoding="utf-8") as f:
        f.write("# ExtendedHUD speed cameras: lat lon limit dir (hud_cam.h). Source: Patrol Police, %s\n" % URL)
        f.write("# %d fixed cameras; dir from OpenStreetMap: %s\n" % (len(rows), ", ".join("%s %d" % kv for kv in sorted(stats.items()))))
        for r, d in zip(rows, dirs):
            f.write("%.6f %.6f %d %d\n" % (r["lat"], r["lon"], r["limit"], d))
    known = sum(1 for d in dirs if d >= 0)
    print("%d cameras -> %s; direction known for %d (%s)" % (len(rows), a.out, known, stats))


if __name__ == "__main__":
    main()
