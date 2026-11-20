"""Host checks for build/hud-worker: the dump stream and the car modes without a DSI library."""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
WORKER = os.path.join(HERE, "..", "build", "hud-worker")


def run(*args, env=None):
    e = dict(os.environ)
    e.pop("ID6_FAKE_MOST", None)
    if env:
        e.update(env)
    return subprocess.run([WORKER, *args], capture_output=True, text=True, timeout=60, env=e)


def frames(*args):
    r = run("--dump", *args)
    assert r.returncode == 0, r.stderr
    lines = [json.loads(x) for x in r.stdout.splitlines()]
    return lines[0], lines[1:], r.stderr


def check_gear_blink():
    # Owner 2026-11-18: past the Shift point (default 6400) only the gear blinks, one flip per frame; under it, it stays.
    _, fs, err = frames("--seconds", "6", "--rtt", "260", "--in-flight", "16", "--layout", "split", "--canvas", "120x72", "--handles", "31,32",
                        "--palette", "3,4,3,4,3,4,4", "--backs", "fill", "--flash-ms", "0", "--shift-colour", "4")
    assert "0 timeouts" in err
    over = [f for f in fs if f["rpm"] >= 6400]
    under = [f for f in fs if 1500 < f["rpm"] < 6300]
    assert len(over) >= 4 and under
    assert all(f["gear"] for f in under), [f["gear"] for f in under]
    shown = [bool(f["gear"]) for f in over]
    assert True in shown and False in shown, shown
    runs = [f for f in fs]
    for a, b in zip(runs, runs[1:]):              # two frames in a row past the point never show the same phase
        if a["rpm"] >= 6400 and b["rpm"] >= 6400 and a["t"] + 2000 > b["t"]:
            assert bool(a["gear"]) != bool(b["gear"]), (a["t"], b["t"])


def check_blink_rate():
    # Owner 2026-11-19: faster frames must not blink faster. At a 10 ms round trip each phase still stays >= 160 ms.
    _, fs, err = frames("--seconds", "6", "--rtt", "10", "--in-flight", "32", "--pipeline", "--layout", "split", "--canvas", "120x72",
                        "--handles", "31,32", "--palette", "3,4,3,4,3,4,4", "--backs", "fill", "--flash-ms", "0", "--shift-colour", "4")
    assert "0 timeouts" in err
    over = [f for f in fs if f["rpm"] >= 6400]
    assert len(over) >= 8 and True in [bool(f["gear"]) for f in over] and False in [bool(f["gear"]) for f in over]
    flips = [b["t"] for a, b in zip(fs, fs[1:]) if a["rpm"] >= 6400 and b["rpm"] >= 6400 and bool(a["gear"]) != bool(b["gear"])]
    assert len(flips) >= 2, flips
    assert all(y - x >= 160 for x, y in zip(flips, flips[1:])), flips


def check_dump():
    head, fs, err = frames("--seconds", "10", "--rtt", "10")
    assert head["handles"] == [5, 6] and head["canvas"] == [480, 180]
    assert "0 timeouts" in err
    assert len(fs) > 80, len(fs)                 # about 11 fps at a 10 ms round trip
    hidden = None
    for f in fs:
        calls = f["calls"]
        assert 0 < len(calls) <= 48
        first, last = calls[0], calls[-1]
        assert first.startswith("220:"), first   # clear the hidden image
        assert last.startswith("20E:"), last     # then switch to it
        h = int(first[4:8], 16)
        assert h in (5, 6)
        assert h != hidden                       # the buffers alternate
        hidden = h
        for c in calls:
            fkt, payload = c.split(":")
            b = bytes.fromhex(payload)
            assert fkt in ("220", "21B", "21D", "20E"), c
            if fkt == "21D":
                assert b[4] == 3 and len(b) == 4 * b[5] + 6
            if fkt != "20E":
                assert int.from_bytes(b[:2], "big") == h
    # The pull reaches the shift point, so some frames light every field and use no unlit colour, and the
    # flash's off phase uses only unlit colours.
    off = {head["palette"][i] for i in (0, 2, 4)}
    def colours(f):
        return {int(c[8:10], 16) for c in f["calls"] if c.startswith("21B:")}
    assert any(not (colours(f) & off) and sum(c.startswith("21D:") for c in f["calls"]) >= 12 for f in fs)
    assert any(colours(f) == off for f in fs if f["rpm"] >= 6395)   # the flash starts at 6,400
    _, fs_static, _ = frames("--seconds", "10", "--rtt", "10", "--static-handle", "7")
    assert len(fs_static) > len(fs)


def check_story():
    r = run("--dump", "--trace", "drive", "--rtt", "10")
    assert r.returncode == 0, r.stderr
    lines = [json.loads(x) for x in r.stdout.splitlines()][1:]
    events = [x for x in lines if "event" in x]
    frames_ = [x for x in lines if "calls" in x]
    assert len(events) == 12
    assert {f["scene"] for f in frames_} == {"rpm", "none"}

    def scenes_between(a, b):
        return {f["scene"] for f in frames_ if a <= f["t"] < b}
    assert scenes_between(0, 14000) == set()            # Comfort: we draw nothing
    assert "rpm" in scenes_between(14000, 30000)        # Sport, Sport+
    assert scenes_between(48000, 58000) == {"rpm"}     # Always, also after Comfort
    assert scenes_between(58000, 64000) == {"none"}    # factory navigation owns the HUD
    assert scenes_between(68100, 100000) <= {"none"}   # Always off in Comfort, Eco Pro, Sport Bar off
    assert "rpm" in scenes_between(100000, 118000)     # Sport Bar on again in Sport+
    assert all("nav" not in e["settings"] for e in events)   # Navigation lives on the feature/nav branch
    # The scene changes only on an event: no switching back and forth on its own.
    ev_t = [e["t"] for e in events]
    last_scene, since = None, 0
    for f in frames_:
        if f["scene"] != last_scene:
            assert last_scene is None or any(since <= t <= f["t"] for t in ev_t), (since, f["t"], f["scene"])
            last_scene, since = f["scene"], f["t"]


def check_car_modes_without_dsi():
    r = run("--get-size")
    assert r.returncode == 1 and "MOST open failed: -3" in r.stderr, r.stderr
    r = run("--draw-live")
    assert r.returncode == 2                     # --udp-port is required
    r = run("--palette", "1,2,3", "--dump")
    assert r.returncode == 2


def check_camera():
    # Speed-camera alert (owner 2026-11-19): from 500 m before the camera to 50 m after it; the whole picture without the bar
    # (redrawn every 10 m: the distance bar), with the bar a badge and the limit in the revs' place; the camera in the red.
    _, fs, err = frames("--scene", "cam", "--rtt", "260", "--in-flight", "32", "--pipeline", "--layout", "split",
                        "--canvas", "120x72", "--handles", "31,32", "--palette", "3,4,3,4,3,4,4", "--red", "9")
    assert "0 timeouts" in err
    whole = [f for f in fs if f["t"] < 30000 and len(f["calls"]) > 4]
    assert 40 <= len(whole) <= 60 and all(len(f["calls"]) <= 16 for f in whole), [len(f["calls"]) for f in whole]
    first, last = whole[0]["t"], whole[-1]["t"]
    assert 3900 <= first <= 4300 and 23800 <= last <= 24300, (first, last)   # 600 m -> 500 m at 25 m/s: 4 s; at it: 24 s
    gone = [f["t"] for f in fs if 24300 < f["t"] < 30000 and f["calls"][0].startswith("220:")]
    assert gone and 25800 <= gone[0] <= 26300, gone                              # 50 m past: cleared
    assert all(any(c == "21B:%04x09" % int(f["calls"][0][4:8], 16) for c in f["calls"]) for f in whole)
    bar = [f for f in fs if 30000 <= f["t"] < 60000 and f["scene"] == "rpm"]
    badge = [f for f in bar if any(c.endswith("09") and c.startswith("21B:") for c in f["calls"])]
    assert badge and all(len(f["calls"]) <= 20 for f in badge)
    assert any(f["t"] > 56000 or not any(c.startswith("21B:") and c.endswith("09") for c in f["calls"]) for f in bar[-1:])
    # Orange camera warning and the 800 m Warning distance (iDrive menu, owner 2026-11-20), from settings.txt
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as t:
        t.write("cam_dist=800\ncam_orange=1\n")
    try:
        head, fs, err = frames("--scene", "cam", "--rtt", "260", "--in-flight", "32", "--pipeline", "--layout", "split",
                               "--canvas", "120x72", "--handles", "31,32", "--palette", "3,4,3,4,3,4,4",
                               "--settings", t.name)
    finally:
        os.unlink(t.name)
    assert head["cam_dist"] == 800 and head["cam_ink"] == 5 and "0 timeouts" in err
    pass_ms = (800 + 180) * 40                               # 39.2 s a pass at 25 m/s, from 900 m to 80 m past
    whole = [f for f in fs if f["t"] < pass_ms and len(f["calls"]) > 4]
    first, last = whole[0]["t"], whole[-1]["t"]
    assert 3900 <= first <= 4300 and 35800 <= last <= 36300, (first, last)   # 900 m -> 800 m: 4 s; at it: 36 s
    assert all(any(c == "21B:%04x05" % int(f["calls"][0][4:8], 16) for c in f["calls"]) for f in whole)


if __name__ == "__main__":
    check_gear_blink()
    check_blink_rate()
    check_dump()
    check_story()
    check_car_modes_without_dsi()
    check_camera()
    print("hud worker tests passed")
    sys.exit(0)
