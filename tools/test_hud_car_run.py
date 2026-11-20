import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from hud_car_run import STAGE, STEPS, Session, cksum, load_files, parse_log, worker_args

LOG = """[hud] 82/00 up, host 0101
[hud] tx GetSize 5
[hud] rx C04 op C len 6: 05 01 01 e0 00 b4
[hud] rpm 6400 gear 0: batches 12 done 12 replaced 0 failed 0, calls 200 answers 200 errors 0 timeouts 0 strays 0, batch 41 ms (max 55)
[car-run] rc=0
"""


class FakeHead:
    """A head unit that answers the commands the session sends, and records them."""

    def __init__(self, node="hu-omap", others=""):
        self.node, self.others, self.files, self.sent, self.tmp = node, others, {}, [], []

    def __call__(self, command, stdin=None, timeout=60, stream=False):
        self.sent.append(command)
        if command == "uname -a":
            return 0, f"QNX {self.node} 6.5.0 2015/12/01 J5_Evo armle\n"
        if command.endswith("pidin arg"):
            server = "DSYSMOSTServer" if ("on -f" in command) == ("jacinto" in self.node) else ""
            return 0, f"  1 procnto\n 77 {server}\n{self.others}"
        if command.startswith("rm "):
            self.files.pop(command.split()[-1], None)
            return 0, ""
        if command.startswith("cp /bin/cat "):
            self.files[command.split()[-1]] = b""  # seed: mode 0555, contents replaced next
            return 0, ""
        if command.startswith("cat > "):
            self.files[command.split()[2]] = stdin
            return 0, ""
        if command.startswith("cksum "):
            data = self.files[command.split()[1]]
            return 0, f"{cksum(data)} {len(data)} {command.split()[1]}\n"
        if "hud-sportbar-qnx --" in command or "hud-shape-qnx --" in command:
            return 0, LOG
        if command.startswith("ls " + STAGE):
            return 1, "No such file"
        if command.endswith("ls -a /tmp"):
            return 0, " ".join([".", "..", "dsi.sock"] + self.tmp) + "\n"
        return 0, ""


class CarRunTest(unittest.TestCase):
    def session(self, head, folder):
        s = Session(Path(folder), ssh=head, confirm=lambda _p: True, out=lambda _t: None)
        s.state["head"] = "169.254.1.2"
        return s

    def test_cksum_matches_posix_cksum(self):
        for data in (b"", b"123456789", os.urandom(70000)):
            r = subprocess.run(["cksum"], input=data, capture_output=True, check=True)
            self.assertEqual(int(r.stdout.split()[0]), cksum(data))

    def test_parse_log(self):
        found = parse_log(LOG)
        self.assertEqual(found["host"], "82/00 host 0101")
        self.assertEqual(found["c04"], ["op C: 05 01 01 e0 00 b4"])
        self.assertEqual(found["stats"], "errors 0, timeouts 0, batch 41 ms (max 55)")
        self.assertEqual(found["rc"], 0)
        packaged = parse_log("batch 'gauges': 12 calls answered in 40 ms\nbatch 'gauges': 12 calls answered in 56 ms\n")
        self.assertEqual(packaged["rtt_ms"], 12.0)
        self.assertEqual(parse_log("[hud] MOST open failed: -1 (...)")["most"], "MOST open failed: -1")

    def test_run2_steps(self):
        self.assertEqual(worker_args(STEPS["R4"], {"handles": "31,32", "canvas": "300x200"})[:5],
                         ["--grid", "--show", "both", "--container", "23"])
        self.assertIn("--container", worker_args(STEPS["R8"], {"handles": "31,32", "container": "23"}))
        self.assertEqual(worker_args(STEPS["R4"], {"container": "23"}).count("--container"), 1)
        # Run 2: R6 went out without it and the HUD (82/02) stayed blank.
        self.assertIn("--container", worker_args(STEPS["R6"], {"handles": "31,32", "container": "23"}))
        self.assertNotIn("--container", worker_args(STEPS["R5"], {"container": "23"}))
        self.assertNotIn("--container", worker_args(STEPS["R1"], {"container": "23"}))
        # Run 3 steps on the HUD pair: the container goes to the bars and the draws, the palette to the draws only.
        state = {"inst": "2", "handles": "31,32", "canvas": "120x72", "container": "23", "palette": "3,4,3,5,3,8,4"}
        for k in ("S0f", "S0l", "S1"):
            self.assertEqual(worker_args(STEPS[k], state).count("--container"), 1, k)
        self.assertIn("--palette", worker_args(STEPS["S1"], state))
        self.assertNotIn("--palette", worker_args(STEPS["S0f"], state))
        # Run 3, S0f as logged: the worker's alarm covered one page, not eight, and killed it mid-way (rc 142).
        log = ("[hud] 82/02 up, host 0101\n[hud] page 5 at 34.3 s: fill colours 32..39 left to right, 2273 ms, 0 errors,"
               " 0 timeouts\nAlarm clock \n[car-run] rc=142\n")
        self.assertEqual(parse_log(log)["rc"], 142)
        src = (Path(__file__).resolve().parents[1] / "native/src/hud/hud_worker.c").read_text()
        self.assertIn("o.swatch_pages : 1) + 30", src)
        # Run 4: steps that name their own pair or palette keep it.
        t1 = worker_args(STEPS["T1a"], state)
        self.assertEqual(t1.count("--handles"), 1)
        self.assertEqual(t1[t1.index("--handles") + 1], "33,34")
        t2 = worker_args(STEPS["T2"], state)
        self.assertEqual(t2[t2.index("--palette") + 1], "3,3,3,3,3,3,4")
        self.assertEqual(t2.count("--palette"), 1)
        self.assertEqual(worker_args(STEPS["T1b"], state).count("--container"), 1)
        for k in ("S0f", "S0l", "T0"):
            a = STEPS[k].args
            self.assertGreaterEqual(STEPS[k].seconds, int(a[a.index("--seconds") + 1]) * int(a[a.index("--swatch-pages") + 1]))

    def test_replayed_run1_logs_flag_dropped_answers(self):
        # Car run 1 (2026-11-17), A3b and B1 as logged: the unit answered with op D, but the old sender
        # counted none, timed out and cut every batch after 3 calls. The report must say so.
        a3b = ("[hud] 82/00 up, host 0101\n"
               "[hud] rx C04 op D len 6: 00 01 01 46 01 46\n[hud] rx C04 op D len 6: 00 01 01 46 01 46\n"
               "[hud] rx C04 op D len 6: 00 02 01 2f 00 29\n"
               "[hud] probe-sizes 1..64: 0 answers, 0 errors, 3 timeouts. The rx C04 lines ...\n[car-run] rc=1\n")
        b1 = ("[hud] rx C04 op D len 6: 00 05 00 7e 00 7e\n[hud] rx C04 op D len 6: 00 06 00 7e 00 7e\n"
              "[hud] get-size: 0 answers, 0 errors, 1 timeouts\n")
        for log in (a3b, b1):
            self.assertIn("answers dropped", parse_log(log)["flags"])
        healthy = ("[hud] rx C04 op D len 6: 00 05 00 7e 00 7e\n[hud] rx C04 op D len 6: 00 06 00 7e 00 7e\n"
                   "[hud] get-size: 2 answers, 0 errors, 0 timeouts\n")
        self.assertNotIn("flags", parse_log(healthy))
        # A draw step that times out is a silent terminal or a dropped answer, never "accepted".
        self.assertIn("timeouts", parse_log("[hud] grid on 412x329: 0 errors, 1 timeouts. Photograph\n")["flags"])

    def test_worker_args_use_session_values(self):
        state = {"palette": "1,2,3,4,5,6,7", "canvas": "400x160", "inst": "2", "handles": "7,8"}
        b3 = worker_args(STEPS["B3"], state)
        self.assertEqual(b3[b3.index("--palette") + 1], "1,2,3,4,5,6,7")
        self.assertEqual(b3[b3.index("--canvas") + 1], "400x160")
        self.assertEqual(b3[b3.index("--most-inst") + 1], "2")
        probe = worker_args(STEPS["A3b"], state)
        self.assertEqual(probe.count("--most-inst"), 1)   # the probe keeps its own instance
        self.assertNotIn("--canvas", probe)
        self.assertNotIn("--palette", worker_args(STEPS["B3"], {"palette": "default"}))

    def test_single_node_flow(self):
        head = FakeHead()
        with tempfile.TemporaryDirectory() as d:
            s = self.session(head, d)
            s.run("A2")
            self.assertEqual(s.state["runner"], "local")
            s.stage({"hud-sportbar-qnx": b"\x7fELF" + os.urandom(1000)})
            s.state["palette"] = "default"
            s.run("B3")
            self.assertIn(f"cd {STAGE} && ./hud-sportbar-qnx --draw-parked --seconds 30", head.sent[-1])
            self.assertEqual(s.state["steps"]["B3"]["found"]["rc"], 0)
            s.state["notes"]["A4"] = ["factory HUD turns orange at night"]
            s.report()
            results = (Path(d) / "results.md").read_text()
            self.assertIn("| B3 |", results)
            self.assertIn("| A4 | owner's check |", results)
            s.cleanup()
            self.assertIn(f"rm -rf {STAGE}", head.sent)

    def test_cleanup_checks_tmp_against_the_baseline(self):
        head = FakeHead(node="hu-jacinto")
        with tempfile.TemporaryDirectory() as d:
            s = self.session(head, d)
            s.run("A2")
            self.assertEqual(s.state["baseline"]["omap"], [".", "..", "dsi.sock"])
            s.stage({"hud-sportbar-qnx": b"x"})
            s.run("A2")                          # a re-run after staging keeps the first baseline
            self.assertNotIn("hud-run", s.state["baseline"]["local"])
            head.tmp = ["hud-trace.txt"]         # something of ours left behind
            with self.assertRaises(SystemExit) as e:
                s.cleanup()
            self.assertIn("hud-trace.txt", str(e.exception))
            head.tmp = []
            s.cleanup()                          # clean now: no raise

    def test_cleanup_without_baseline_is_not_called_clean(self):
        head = FakeHead()
        with tempfile.TemporaryDirectory() as d:
            s = self.session(head, d)
            with self.assertRaises(SystemExit) as e:
                s.cleanup()
            self.assertIn("NOT RECORDED", str(e.exception))

    def test_split_node_runs_worker_on_omap_from_qnet(self):
        head = FakeHead(node="hu-jacinto")
        with tempfile.TemporaryDirectory() as d:
            s = self.session(head, d)
            s.run("A2")
            self.assertEqual(s.state["runner"], "omap")
            s.stage({"hud-sportbar-qnx": b"x"})
            s.run("B1")
            self.assertTrue(head.sent[-1].startswith("on -f hu-omap sh -c "))
            self.assertIn("/net/hu-jacinto/tmp/hud-run", head.sent[-1])

    def test_other_sender_blocks_worker_steps(self):
        head = FakeHead(others=" 90 /var/opt/hmi/data/ID5/Plugins/HudTest/hud-shape-qnx --draw-live\n")
        with tempfile.TemporaryDirectory() as d:
            s = self.session(head, d)
            s.run("A2")
            s.stage({"hud-sportbar-qnx": b"x"})
            with self.assertRaises(SystemExit) as e:
                s.run("B1")
            self.assertIn("another HUD sender", str(e.exception))
            self.assertFalse(any("--get-size" in c for c in head.sent))

    def test_unconfirmed_step_sends_nothing(self):
        head = FakeHead()
        with tempfile.TemporaryDirectory() as d:
            asked = []
            # The owner approves only the copy to /tmp (a write); read-only steps must not ask at all.
            s = Session(Path(d), ssh=head, confirm=lambda p: asked.append(p) or p.startswith("Copy"), out=lambda _t: None)
            s.state["head"] = "169.254.1.2"
            s.run("A2")                          # read-only: no asking
            s.stage({"hud-sportbar-qnx": b"x"})
            s.run("R1")                          # size probe: read-only too
            self.assertTrue(any("--probe-sizes" in c for c in head.sent))
            with self.assertRaises(SystemExit):
                s.run("B1b")             # a draw step needs the owner's yes
            self.assertFalse(any("--grid" in c for c in head.sent))
            self.assertEqual([p[:4] for p in asked], ["Copy", "B1b:"])

    def test_only_query_steps_are_readonly(self):
        drawing = ("--grid", "--swatches", "--draw-parked", "--draw-live", "--check-parked")
        for k, st in STEPS.items():
            if st.readonly:
                self.assertFalse(any(a in st.args for a in drawing), k)
            elif st.kind == "ours":
                self.assertTrue(any(a in st.args for a in drawing), k)

    def test_steps_need_staging_and_values(self):
        head = FakeHead()
        with tempfile.TemporaryDirectory() as d:
            s = self.session(head, d)
            s.run("A2")
            with self.assertRaises(SystemExit):
                s.run("B1")          # not staged
            s.stage({"hud-sportbar-qnx": b"x"})
            with self.assertRaises(SystemExit):
                s.run("B3")          # no palette yet

    def _make_packaged_zip(self, folder, worker_bytes=b"\x7fELF\x01\x01", gw_bytes=b"169.254.21.3\n", corrupt=False):
        zip_path = Path(folder) / "hud-gauges.zip"
        worker_sum = hashlib.sha256(b"tampered" if corrupt else worker_bytes).hexdigest()
        gw_sum = hashlib.sha256(gw_bytes).hexdigest()
        sums = f"{worker_sum}  HudTest/hud-shape-qnx\n{gw_sum}  HudTest/gateway-ip.txt\n"
        with zipfile.ZipFile(zip_path, "w") as z:
            z.writestr("HudTest/hud-shape-qnx", worker_bytes)
            z.writestr("HudTest/gateway-ip.txt", gw_bytes)
            z.writestr("SHA256SUMS", sums)
        return zip_path

    def test_packaged_worker_is_bounded_and_checked(self):
        with tempfile.TemporaryDirectory() as d:
            zip_path = self._make_packaged_zip(d)
            files = load_files("packaged", package=zip_path)
            self.assertEqual(set(files), {"hud-shape-qnx", "gateway-ip.txt"})
            head = FakeHead()
            s = self.session(head, d)
            s.run("A2")
            s.stage(files)
            s.run("A3d")
            self.assertIn("kill -INT $p", head.sent[-1])
            self.assertIn("[ $i -lt 20 ]", head.sent[-1])

    def test_packaged_missing_raises_system_exit(self):
        with patch.dict(os.environ, {}, clear=True):
            with self.assertRaises(SystemExit) as e:
                load_files("packaged")
            self.assertIn("--package", str(e.exception))

    def test_packaged_bad_sha256_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            zip_path = self._make_packaged_zip(d, corrupt=True)
            with self.assertRaises(SystemExit) as e:
                load_files("packaged", package=zip_path)
            self.assertIn("sha256", str(e.exception))



class CallTimesTest(unittest.TestCase):
    def test_round_trip_per_call_from_stamped_log(self):
        # Car run 6 log shape (worker --log, 2026-11-18): tx/rx with t=ms; answers matched by function and handle.
        log = ("[hud] tx 220 ImageRemoveAllItems 32 2 0020 t=100\n"
               "[hud] tx 21B ImageSetFillColor 32 3 002003 t=101\n"
               "[hud] rx 220 op D len 2: 00 20 t=360\n"
               "[hud] tx 21D ImageAddPolygon 32 22 0020000103040009 t=361\n"
               "[hud] rx 21B op D len 3: 00 20 03 t=370\n"
               "[hud] rx 21D op D len 22: 00 20 00 01 t=640\n")
        self.assertEqual(parse_log(log)["call_ms"], "3 calls: median 269 ms, max 279 ms")

if __name__ == "__main__":
    unittest.main()
