"""Host checks for build/hud-worker: the dump stream and the car modes without a DSI library."""
import json
import os
import subprocess
import sys

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


def text_of(call):
    b = bytes.fromhex(call.split(":")[1])
    return b[3:-2].decode("utf-16-be")


def check_nav():
    head, fs, err = frames("--scene", "nav", "--rtt", "10")
    assert head["text_handles"] == [2, 3] and "0 timeouts" in err
    texts = set()
    for f in fs:
        assert f["scene"] == "nav" and 0 < len(f["calls"]) <= 48
        for c in f["calls"]:
            assert c[:3] in ("210", "220", "21B", "21D", "20E"), c
            if c.startswith("210:"):
                texts.add(text_of(c))
    for want in ("просп. Берестейський", "вул. Хрещатик", "Південний міст", "вул. Велика Васильк.", "траса М-06",
                 "вул. Сагайдачного 25", "Ви прибули", "1.5 км", "Зараз"):
        assert want in texts, want
    assert all(len(t) <= 20 and "," not in t for t in texts)   # the cluster drops commas
    # The Latin fallback and the international route: only ASCII, and the hostile texts come out clean.
    _, fs, _ = frames("--scene", "nav", "--rtt", "10", "--text", "latin")
    latin = {text_of(c) for f in fs for c in f["calls"] if c.startswith("210:")}
    assert "prosp. Beresteiskyi" in latin and "Vy prybuly" in latin and all(t.isascii() for t in latin), latin
    head, fs, err = frames("--scene", "nav", "--rtt", "10", "--route", "intl", "--lang", "en")
    assert head["route"] == "intl" and head["lang"] == "en" and "0 timeouts" in err
    intl = {text_of(c) for f in fs for c in f["calls"] if c.startswith("210:")}
    for want in ("Str. des 17. Juni", "ul. Marszałkowska", "Ave des Champs-Élys.", 'вул. "Київська"', "Обхід ремонту",
                 "Oxford St", "Kärntner Str. 12", "Arrived", "1.2 km"):
        assert want in intl, (want, intl)
    assert all(0x20 <= ord(ch) < 0xFFFE and ch not in "\u200e\u00a0" for t in intl for ch in t)


def check_story():
    r = run("--dump", "--trace", "drive", "--rtt", "10")
    assert r.returncode == 0, r.stderr
    lines = [json.loads(x) for x in r.stdout.splitlines()][1:]
    events = [x for x in lines if "event" in x]
    frames_ = [x for x in lines if "calls" in x]
    assert len(events) == 16
    assert {f["scene"] for f in frames_} == {"rpm", "nav", "none"}

    def scenes_between(a, b):
        return {f["scene"] for f in frames_ if a <= f["t"] < b}
    assert scenes_between(0, 4000) == set()            # Comfort, no route: we draw nothing
    assert scenes_between(4000, 14000) == {"nav"}      # Comfort with a route
    assert scenes_between(14000, 68000) <= {"rpm", "none"}   # Sport, Sport+, Always: never navigation
    assert "rpm" in scenes_between(14000, 30000)
    assert scenes_between(48000, 58000) == {"rpm"}     # Always, also after Comfort
    assert scenes_between(58000, 64000) == {"none"}    # factory navigation owns the HUD
    assert scenes_between(76000, 86000) <= {"none"}    # Navigation off, Eco Pro
    assert scenes_between(86000, 100000) == {"nav"}    # Sport Bar off: navigation even in Sport+
    assert scenes_between(100100, 118000) <= {"rpm"}   # Sport Bar on again in Sport+: the bar, near turns too
    paused = [e for e in events if e["paused"]]
    assert paused and all(e["settings"]["nav"] == 1 for e in paused)   # Always pauses, never unticks
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


if __name__ == "__main__":
    check_dump()
    check_nav()
    check_story()
    check_car_modes_without_dsi()
    print("hud worker tests passed")
    sys.exit(0)
