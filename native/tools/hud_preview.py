"""Desk previewer for ExtendedHUD (the HUD Sport Bar and the speed-camera warning).

Runs build/hud-worker --dump and writes one self-contained HTML page that replays the exact HUD terminal
calls through a simulated terminal (images and text areas per handle, pen colour, SwitchVisibility), at the
timing the simulated round trip gives. Three tabs:

  Drive story   --trace drive: menu settings, driving modes and the factory navigation change over two
                minutes, and the worker's scene logic decides what the HUD shows
  Sport Bar     --trace pull as installed (the split bar, the only layout)
  Speed camera  --scene cam: a camera passed alone (the whole picture) and next to the Sport Bar (badge + limit)

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
    a = ap.parse_args()
    # the plugin's own flags (hud_app_worker_argv)
    split = ["--layout", "split", "--flash-ms", "0", "--palette", "3,4,3,4,3,4,4", "--shift-colour", "4", "--backs", "fill",
             "--canvas", "120x72", "--handles", "31,32", "--in-flight", "32", "--pipeline", "--rtt", "260"]
    runs = [dump("story", "10 ms round trip", ["--trace", "drive", "--rtt", "10"]),
            dump("rpm", "As installed (split bar, 260 ms round trip)", ["--trace", "pull", "--seconds", "20", *split]),
            dump("cam", "Camera pass · as installed (split bar, 260 ms round trip)", ["--scene", "cam", *split])]
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
