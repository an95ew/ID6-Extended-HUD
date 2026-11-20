"""cameras_build.py: the police table parser and the direction from OpenStreetMap roads (story speed-cameras)."""
import math
import unittest

from cameras_build import directions, parse_table
from hud_car_run import STEPS, worker_args

LAT, LON = 50.45, 30.50


def at(north, east):
    return {"lat": LAT + north / 111320.0, "lon": LON + east / (111320.0 * math.cos(math.radians(LAT)))}


def way(points, **tags):
    return {"type": "way", "tags": {"highway": "primary", **tags}, "geometry": [at(n, e) for n, e in points]}


def cam(north, east, limit=50):
    p = at(north, east)
    p["limit"] = limit
    return p


def osm_cam(north, east, direction, **tags):
    p = at(north, east)
    return {"type": "node", "id": int(abs(north * 1000 + east * 10)) + 1, "lat": p["lat"], "lon": p["lon"],
            "tags": dict(highway="speed_camera", direction=direction, **tags)}


class CamerasBuildTest(unittest.TestCase):
    def test_table(self):
        page = ("<table><tr><th>№</th></tr>"
                "<tr><td>1</td><td></td><td>м. Київ</td><td></td><td>вул. Олени Теліги, 37</td><td>50</td>"
                "<td>50.479648646, 30.453529090</td></tr>"
                "<tr><td>75</td><td></td><td>м. Луцьк</td><td></td><td>вул. Конякіна, 18-А</td><td>50</td>"
                "<td>50, 769657767, 25, 357070027</td></tr>"
                "<tr><td>9</td><td>М-06</td><td>Київська область</td><td>12+300</td><td>x</td><td>110</td><td>-</td></tr>"
                "</table>")
        rows = parse_table(page)
        self.assertEqual(len(rows), 2)                                 # no GPS: skipped
        self.assertAlmostEqual(rows[1]["lat"], 50.769657767)           # commas as decimal points
        self.assertEqual(rows[0]["limit"], 50)

    def test_directions(self):
        road = way([(-500, 0), (500, 0)])                                # two-way, drawn northwards
        oneway_s = way([(500, 300), (-500, 300)], oneway="yes")           # a carriageway running south
        rows = [cam(0, 8), cam(40, -8),                                   # a pair: east side watches north, west side south
                cam(200, 6),                                              # a single on the east side: north
                cam(-400, 0.5),                                           # on the centre line, alone: unknown
                cam(0, 304),                                              # on the one-way carriageway: south
                cam(0, 1000)]                                             # no road near: unknown
        dirs, stats = directions(rows, [road, oneway_s])
        self.assertEqual(dirs[0] % 360, 0)
        self.assertEqual(dirs[1], 180)
        self.assertEqual(dirs[2] % 360, 0)
        self.assertEqual(dirs[3], -1)
        self.assertEqual(dirs[4], 180)
        self.assertEqual(dirs[5], -1)
        self.assertEqual(directions(rows, [])[0], [-1] * len(rows))      # no OSM: all unknown, shown both ways

    def test_directions_osm_cameras_and_edge_cases(self):
        # 1. Non-drivable ways (footway/service) must not hijack a camera on a primary road.
        main_n = way([(-200, 0), (0, 0), (200, 0)], highway="primary")
        sidewalk = way([(-200, 7), (200, 7)], highway="footway")
        side_res = way([(0, 0), (0, 100)], highway="residential")          # side street 3 m away vs primary 8 m away
        dirs, _ = directions([cam(3, 8)], [main_n, sidewalk, side_res])
        self.assertEqual(dirs[0], 0)

        # 2. Anti-parallel OSM segments of one two-way road: pair still splits into opposite directions.
        seg_n = way([(-200, 0), (0, 0)], highway="primary")                # drawn north (0)
        seg_s = way([(200, 0), (0, 0)], highway="primary")                 # drawn south (180)
        dirs, _ = directions([cam(-20, 6), cam(20, -6)], [seg_n, seg_s])
        self.assertEqual(dirs, [0, 180])

        # 3. OSM highway=speed_camera forward/backward overrides opposite-side pole; direction=both -> -1;
        #    numeric direction (ambiguous lens azimuth) falls through to one-way carriageway.
        tw = way([(-200, 0), (0, 0), (50, 0), (200, 0)], highway="primary")
        ow = way([(-200, 500), (0, 500), (200, 500)], highway="trunk", oneway="yes")
        elems = [
            tw, ow,
            osm_cam(0, 0, "backward"),     # road runs north (0), camera controls southbound (180), even if pole is on east (+6m)
            osm_cam(50, 0, "both"),        # explicitly bidirectional -> -1 even if pole is on east (+6m)
            osm_cam(0, 500, "180"),        # numeric lens azimuth on northbound one-way -> must keep one-way 0
        ]
        dirs, _ = directions([cam(0, 6), cam(50, 6), cam(0, 505)], elems)
        self.assertEqual(dirs, [180, -1, 0])

        # 4. 1-to-1 greedy matching: two nearby police rows do not both take the same OSM speed_camera node.
        ow_n = way([(-200, 0), (0, 0), (200, 0)], highway="trunk", oneway="yes")
        ow_s = way([(200, -12), (0, -12), (-200, -12)], highway="trunk", oneway="yes")
        elems = [ow_n, ow_s, osm_cam(0, 0, "forward"), osm_cam(0, -12, "forward")]
        # Both rows sit slightly closer to the median (x=-4 and x=-8): 1-to-1 assigns one to 0 and one to 180.
        dirs, _ = directions([cam(0, -4), cam(0, -8)], elems)
        self.assertEqual(dirs, [0, 180])

        # 5. Identical-coordinate median pair on a divided highway splits into opposite directions.
        dirs, stats = directions([cam(0, -5), cam(0, -5)], [ow_n, ow_s])
        self.assertEqual(sorted(dirs), [0, 180])
        self.assertEqual(sum(stats.values()), 2)

    def test_pair_completion(self):
        # A camera 33 m off the fetched road (unknown) next to a directed one-way camera: the opposite way.
        ow = way([(-200, 0), (200, 0)], highway="primary", oneway="yes")
        dirs, stats = directions([cam(0, 3), cam(60, 33)], [ow])
        self.assertEqual(dirs, [0, 180])
        self.assertEqual(stats["pair completion"], 1)
        # OSM direction=both stays both ways; a lone unknown camera stays unknown.
        dirs, _ = directions([cam(0, 3), cam(50, 3)], [ow, osm_cam(50, 0, "both")])
        self.assertEqual(dirs, [0, -1])
        dirs, _ = directions([cam(0, 33)], [ow])
        self.assertEqual(dirs, [-1])

    def test_car_steps(self):
        k = worker_args(STEPS["K1"], {"handles": "31,32", "canvas": "120x72", "container": "23", "inst": "2", "red": "9"})
        self.assertIn("cam", k)
        self.assertNotIn("--red", k)                                      # K1 is the look without red
        kr = worker_args(STEPS["K1r"], {"handles": "31,32", "canvas": "120x72", "red": "9"})
        self.assertEqual(kr[kr.index("--red") + 1], "9")
        self.assertIn("--swatch-pages", STEPS["P1"].args)


if __name__ == "__main__":
    unittest.main()
