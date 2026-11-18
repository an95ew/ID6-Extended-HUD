"""Desk previewer for ExtendedHUD (the HUD Sport Bar and nav scenes).

Runs build/hud-worker --dump and writes one self-contained HTML page that replays the exact HUD terminal
calls through a simulated terminal (images and text areas per handle, pen colour, SwitchVisibility), at the
timing the simulated round trip gives. Three tabs:

  Drive story   --trace drive: menu settings, driving modes, a CarPlay route and the factory navigation
                change over two minutes, and the worker's scene logic decides what the HUD shows
  Sport Bar     --trace pull at several round trips
  Navigation    --scene nav: the Ukrainian route (Cyrillic, and the Latin fallback) and the international
                route (other languages and hostile text from the phone, six lanes)

    python3 tools/hud_preview.py -o /tmp/hud-preview.html
"""
import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
WORKER = os.path.join(HERE, "..", "build", "hud-worker")
TEMPLATE = os.path.join(HERE, "hud_preview_template.html")


def dump(tab, label, args):
    r = subprocess.run([WORKER, "--dump", *args], capture_output=True, text=True, check=True)
    lines = [json.loads(x) for x in r.stdout.splitlines()]
    return {
        "tab": tab,
        "label": label,
        "head": lines[0],
        "frames": [x for x in lines[1:] if "calls" in x],
        "events": [x for x in lines[1:] if "event" in x],
        "summary": r.stderr.strip(),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--rtt", action="append", default=None,
                    help="round trip in ms for the Sport Bar tab; repeat to compare (default 5, 10, 20)")
    a = ap.parse_args()
    runs = [dump("story", "10 ms round trip", ["--trace", "drive", "--rtt", "10"])]
    for rtt in a.rtt or ["5", "10", "20"]:
        runs.append(dump("rpm", f"{rtt} ms round trip", ["--trace", "pull", "--seconds", "20", "--rtt", rtt]))
    runs.append(dump("nav", "Ukraine · Cyrillic", ["--scene", "nav", "--rtt", "10"]))
    runs.append(dump("nav", "Ukraine · Latin fallback (HUD without Cyrillic)",
                     ["--scene", "nav", "--rtt", "10", "--text", "latin"]))
    runs.append(dump("nav", "Other languages · English units",
                     ["--scene", "nav", "--rtt", "10", "--route", "intl", "--lang", "en"]))
    with open(TEMPLATE, encoding="utf-8") as f:
        page = f.read()
    page = page.replace("/*__RUNS__*/[]", json.dumps(runs, ensure_ascii=False, separators=(",", ":")))
    with open(a.out, "w", encoding="utf-8") as f:
        f.write(page)
    for r in runs:
        print(f"{r['tab']} {r['label']}: {r['summary']}")
    print(f"wrote {a.out}")


if __name__ == "__main__":
    sys.exit(main())
