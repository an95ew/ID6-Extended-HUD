"""cameras_build.py: the police table parser and the direction from OpenStreetMap roads (story speed-cameras)."""
import math
import unittest

from cameras_build import directions, parse_table
from hud_car_run import STEPS, worker_args

LAT, LON = 50.45, 30.50


def at(north, east):
    return {"lat": LAT + north / 111320.0, "lon": LON + east / (111320.0 * math.cos(math.radians(LAT)))}


def way(points, **tags):
    return {"type": "way", "tags": dict(highway="primary", **tags), "geometry": [at(n, e) for n, e in points]}


def cam(north, east, limit=50):
    p = at(north, east)
    p["limit"] = limit
    return p


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

    def test_car_steps(self):
        k = worker_args(STEPS["K1"], {"handles": "31,32", "canvas": "120x72", "container": "23", "inst": "2", "red": "9"})
        self.assertIn("cam", k)
        self.assertNotIn("--red", k)                                      # K1 is the look without red
        kr = worker_args(STEPS["K1r"], {"handles": "31,32", "canvas": "120x72", "red": "9"})
        self.assertEqual(kr[kr.index("--red") + 1], "9")
        self.assertIn("--swatch-pages", STEPS["P1"].args)


if __name__ == "__main__":
    unittest.main()
