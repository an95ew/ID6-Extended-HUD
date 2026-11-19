#!/usr/bin/env python3
"""Run the parked HUD car session (car-session-1 runbook) step by step over SSH.

Every remote step is confirmed, bounded in time and logged to a private session folder.
Nothing is installed: files are staged under /tmp/hud-run and removed by `cleanup`.
"""

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import threading
import zipfile

from capture_headunit import LEGACY, enet_ip, is_arm_elf

ROOT = Path(__file__).resolve().parents[1]
OURS = ROOT / "native" / "out" / "hud-sportbar-qnx"
PACKAGED = ("HudTest/hud-shape-qnx", "HudTest/gateway-ip.txt")
STAGE = "/tmp/hud-run"
OMAP_ENV = "export LD_LIBRARY_PATH=/opt/sys/lib:/usr/lib:/lib; "
PLUGINS = "/var/opt/hmi/data/ID5/Plugins"
# Two senders on the HUD terminal freeze it (home repo #22/#27), so these block every worker step.
SENDERS = re.compile(r"hud-shape-qnx|hud-sportbar-qnx|wayglass", re.I)
# Shown for the owner to judge, not blocking: factory and other apps near the cluster.
NEARBY = re.compile(r"hud|kombi|NBTEVOEXT|aad|wayglass|carplay", re.I)


class Step:
    def __init__(self, title, kind, args=(), seconds=0, film="", needs=(), readonly=False):
        self.title, self.kind, self.args, self.seconds, self.film, self.needs = title, kind, list(args), seconds, film, needs
        # Read-only steps (capture, inventory, size queries) draw nothing, so they run without asking the owner.
        self.readonly = readonly or kind in ("capture", "inventory")


STEPS = {
    "A1": Step("evidence capture (capture_headunit.py)", "capture"),
    "A2": Step("node, MOST server, installed and running HUD apps", "inventory"),
    "A3": Step("packaged worker --check-parked", "packaged", ["--check-parked"], 20),
    "A3d": Step("packaged worker --draw-parked, 20 s", "packaged", ["--draw-parked"], 20, "Film the HUD while it draws."),
    "A3b": Step("probe sizes on 82/00", "ours", ["--probe-sizes", "1-64", "--most-inst", "0"], 30, readonly=True),
    "A3b2": Step("probe sizes on 82/02", "ours", ["--probe-sizes", "1-64", "--most-inst", "2"], 30, readonly=True),
    "B1": Step("link and canvas size", "ours", ["--get-size", "--log"], 10, readonly=True),
    "B1b": Step("visible area grid", "ours", ["--grid", "--seconds", "20"], 20,
                "Photograph which of the 8x4 markers show."),
    "B2a": Step("colour swatches 0..15", "ours", ["--swatches", "0", "--seconds", "20"], 20,
                "Photograph the 16 squares: index = row x 8 + column."),
    "B2b": Step("colour swatches 16..31", "ours", ["--swatches", "16", "--seconds", "20"], 20,
                "Photograph the 16 squares: index = 16 + row x 8 + column."),
    "B3": Step("M Strip sweep, watch the flash from 6,400", "ours", ["--draw-parked", "--seconds", "30"], 30,
               "Video the sweep; the bar must blink from 6,400.", ("palette",)),
    "B4": Step("navigation scene, Cyrillic", "ours", ["--draw-parked", "--scene", "nav", "--seconds", "40"], 40,
               "Video: street (area 2), distance (area 3), lanes. Does Cyrillic render?"),
    "B4c": Step("navigation scene, Latin fallback", "ours",
                ["--draw-parked", "--scene", "nav", "--text", "latin", "--seconds", "40"], 40,
                "Only if B4 showed boxes or blanks. How many characters fit the street line?"),
    "B4d": Step("navigation scene, hostile texts", "ours",
                ["--draw-parked", "--scene", "nav", "--route", "intl", "--lang", "en", "--seconds", "75"], 75,
                "Video: long names cut cleanly, l/e/A with accents, six lanes."),
    "B5": Step("M Strip on the measured canvas", "ours", ["--draw-parked", "--seconds", "30"], 30,
               "Video: where the bar sits next to the factory speed.", ("palette", "canvas")),
    # Run 2 (car-run-2-plan.md): the sender now takes op D answers; --show/--container bring the image up.
    "R1": Step("probe sizes 1..64 on 82/00, fixed sender", "ours", ["--probe-sizes", "1-64", "--most-inst", "0"], 40, readonly=True),
    "R2": Step("probe sizes 1..64 on 82/02, fixed sender", "ours", ["--probe-sizes", "1-64", "--most-inst", "2"], 40, readonly=True),
    "R3": Step("get-size answers are counted", "ours", ["--get-size", "--log"], 10, readonly=True),
    "R4": Step("grid, SetVisibility + SwitchVisibility + container 0x17", "ours",
               ["--grid", "--show", "both", "--container", "23", "--seconds", "20"], 20,
               "Film the CLUSTER nav area and the HUD. Which 8x4 markers show? Route active or not?", ("handles", "canvas")),
    "R5": Step("grid, SetVisibility + SwitchVisibility, no container", "ours",
               ["--grid", "--show", "both", "--seconds", "20"], 20,
               "Film the CLUSTER and the HUD. Which markers show?", ("handles", "canvas")),
    "R6": Step("colour swatches 0..15 on the pair that showed", "ours",
               ["--swatches", "0", "--show", "both", "--seconds", "20"], 20,
               "Photograph the 16 squares: index = row x 8 + column.", ("handles", "canvas")),
    "R7": Step("colour swatches 16..31 on the pair that showed", "ours",
               ["--swatches", "16", "--show", "both", "--seconds", "20"], 20,
               "Photograph the 16 squares: index = 16 + row x 8 + column.", ("handles", "canvas")),
    "R8": Step("M Strip sweep on the pair that showed", "ours", ["--draw-parked", "--seconds", "30"], 30,
               "Video the sweep; the bar must blink from 6,400.", ("handles", "canvas")),
    "R9": Step("navigation scene on the pair that showed", "ours", ["--draw-parked", "--scene", "nav", "--seconds", "40"], 40,
               "Video: street, distance, lanes. Does Cyrillic render?", ("handles", "canvas")),
    # Run 3 (car-run-3-plan.md): 82/02 31/32 120x72 with container 23, as found in run 2.
    "S0f": Step("big swatch bars, fill colours 0..63, 8 pages of 8", "ours",
                ["--swatches", "0", "--swatch-n", "8", "--swatch-pages", "8", "--show", "both", "--seconds", "8"], 70,
                "Photo each page (8 s each): 8 bars, white feet. Page k shows 8k..8k+7 left to right. Which is red?",
                ("handles", "canvas")),
    "S0l": Step("big swatch bars, line colours 0..63, 8 pages of 8", "ours",
                ["--swatches", "0", "--swatch-n", "8", "--swatch-pages", "8", "--swatch-line", "--show", "both",
                 "--seconds", "8"], 70, "Photo each page: outlined bars in the line colour. Which is red?", ("handles", "canvas")),
    "S1": Step("big M strip, pull 1st-5th with gear digit, steady shift state", "ours",
               ["--draw-parked", "--trace", "pull", "--layout", "big", "--flash-ms", "0", "--shift-hold", "3000",
                "--seconds", "40"], 40, "Video: bigger? gear digit 1..5? shift state (all lit) for ~3 s?",
               ("handles", "canvas")),
    "S1b": Step("big M strip, slow shift blink (2 s), one frame takes ~2.1 s", "ours",
                ["--draw-parked", "--trace", "pull", "--layout", "big", "--flash-ms", "2000", "--shift-hold", "6000",
                 "--seconds", "50"], 50, "Video: does the full bar go dark and lit again at each shift?", ("handles", "canvas")),
    "S2": Step("big navigation scene", "ours", ["--draw-parked", "--scene", "nav", "--layout", "big", "--seconds", "40"], 40,
               "Video: arrow, distance bar, lanes bigger and readable?", ("handles", "canvas")),
    # Run 4 (car-run-4-plan.md), same target as run 3.
    "T0": Step("big bars, line colours 0..15 (outline test)", "ours",
               ["--swatches", "0", "--swatch-n", "8", "--swatch-pages", "2", "--swatch-line", "--show", "both",
                "--seconds", "10"], 25, "Photo both pages: do outlines draw? Which positions?", ("handles", "canvas")),
    "T1a": Step("grid on 82/02 0x21/0x22 (another 120x72 pair)", "ours",
                ["--grid", "--handles", "33,34", "--show", "both", "--container", "23", "--seconds", "15"], 15,
                "Photo the HUD: dots or nothing?", ("canvas",)),
    "T1b": Step("grid on 82/02 0x25 (120x72)", "ours",
                ["--grid", "--handles", "31,37", "--show", "both", "--container", "23", "--seconds", "15"], 15,
                "Photo the HUD: dots or nothing?", ("canvas",)),
    "T1c": Step("grid on 82/02 0x2E (120x72)", "ours",
                ["--grid", "--handles", "31,46", "--show", "both", "--container", "23", "--seconds", "15"], 15,
                "Photo the HUD: dots or nothing?", ("canvas",)),
    "T2": Step("M strip, brightness cue: dim 3 through the revs, all white 4 at the shift, no backs", "ours",
               ["--draw-parked", "--trace", "pull", "--layout", "big", "--flash-ms", "0", "--shift-hold", "3000",
                "--palette", "3,3,3,3,3,3,4", "--shift-colour", "4", "--backs", "none", "--seconds", "40"], 40,
               "Video: is the shift moment obvious at a glance?", ("handles", "canvas")),
    "T2b": Step("M strip, brightness cue with outlined unlit fields", "ours",
                ["--draw-parked", "--trace", "pull", "--layout", "big", "--flash-ms", "0", "--shift-hold", "3000",
                 "--palette", "3,3,3,3,3,3,4", "--shift-colour", "4", "--backs", "outline", "--seconds", "40"], 40,
                "Video: thin outlines for the unlit fields? (only if T0 drew outlines)", ("handles", "canvas")),
    "T3": Step("big nav, texts to 0x1A/0x1B with SetVisibility (6WB text areas, JayJay3071)", "ours",
               ["--draw-parked", "--scene", "nav", "--layout", "big", "--text-handles", "26,27", "--text-visible",
                "--text", "latin", "--seconds", "40"], 40,
               "Video the HUD AND the cluster: any street or distance text? Where?", ("handles", "canvas")),
    "T3c": Step("as T3, Cyrillic", "ours",
                ["--draw-parked", "--scene", "nav", "--layout", "big", "--text-handles", "26,27", "--text-visible",
                 "--seconds", "40"], 40, "Video: does Cyrillic render?", ("handles", "canvas")),
    # Run 5 (car-run-5-plan.md): a dim colour that shows by day (run 4: 3 was invisible), nav clear of the HUD's texts.
    "U0": Step("big swatch bars by day, fill colours 2..11, 2 pages of 5", "ours",
               ["--swatches", "2", "--swatch-n", "5", "--swatch-pages", "2", "--show", "both", "--seconds", "12"], 30,
               "Video both pages: which colours show by day, and which is dimmer than white but still clear?",
               ("handles", "canvas")),
    "U1": Step("M strip, brightness cue with dim 5 (orange) through the revs, all white 4 at the shift, no backs", "ours",
               ["--draw-parked", "--trace", "pull", "--layout", "big", "--flash-ms", "0", "--shift-hold", "3000",
                "--palette", "5,5,5,5,5,5,4", "--shift-colour", "4", "--backs", "none", "--seconds", "40"], 40,
               "Video: fields and gear visible by day? Shift moment still obvious?", ("handles", "canvas")),
    "U2": Step("big nav with texts (Cyrillic), picture below the texts, no bar, dim 5 (roundabout ring)", "ours",
               ["--draw-parked", "--scene", "nav", "--layout", "big", "--text-handles", "26,27", "--text-visible",
                "--palette", "5,4,5,5,5,8,4", "--seconds", "45"], 45,
               "Video: text clear of the arrow? Roundabout ring visible, reads as a roundabout?", ("handles", "canvas")),
    # Owner after U1/U2: no colour inversion, BMW's look = white by day (the HUD tints it orange at night by itself).
    "V1": Step("M strip all white 4 (no dim colour, no backs), full bar at the shift", "ours",
               ["--draw-parked", "--trace", "pull", "--layout", "big", "--flash-ms", "0", "--shift-hold", "3000",
                "--palette", "4,4,4,4,4,4,4", "--shift-colour", "4", "--backs", "none", "--seconds", "40"], 40,
               "Video: OEM-like? Shift moment readable from the shape alone?", ("handles", "canvas")),
    "V2": Step("big nav with Cyrillic texts, all white 4; lanes not to take smaller, no separators", "ours",
               ["--draw-parked", "--scene", "nav", "--layout", "big", "--text-handles", "26,27", "--text-visible",
                "--palette", "4,4,4,4,4,4,4", "--seconds", "45"], 45,
               "Video: OEM-like? Roundabout reads? Which lane to take is clear? Shade around our picture?",
               ("handles", "canvas")),
    # Car run 6 (2026-11-19): latency. The split bar merged into ~7 calls a frame (one polygon per colour and baseline,
    # up to 58 points: run 4 never sent more than 18), then wider windows. --log stamps every tx/rx with t=ms.
    "W1": Step("split bar merged (~7 calls a frame), window 3, pull 1st-5th", "ours",
               ["--draw-parked", "--trace", "pull", "--layout", "split", "--flash-ms", "0", "--palette", "3,4,3,4,3,4,4",
                "--shift-colour", "4", "--backs", "fill", "--log", "--seconds", "30"], 30,
               "Video: every field, the gear and all RPM digits there (0/6/8/9 with holes)? Any thin line along the bar's or the"
               " digits' bottom edge between the pieces?", ("handles", "canvas")),
    "W1b": Step("as W1, one polygon per piece (fallback if W1 draws wrong)", "ours",
                ["--draw-parked", "--trace", "pull", "--layout", "split", "--flash-ms", "0", "--palette", "3,4,3,4,3,4,4",
                 "--shift-colour", "4", "--backs", "fill", "--merge", "0", "--log", "--seconds", "30"], 30,
                "Video: same picture as W1 should have been?", ("handles", "canvas")),
    "W4": Step("a polygon per digit (no line gear-RPM), 16 calls in flight, revs held past 6400 for 2.5 s per gear", "ours",
               ["--draw-parked", "--trace", "pull", "--shift-hold", "2500", "--layout", "split", "--flash-ms", "0",
                "--palette", "3,4,3,4,3,4,4", "--shift-colour", "4", "--backs", "fill", "--in-flight", "16", "--log",
                "--seconds", "40"], 40,
               "Video: no line between the gear and the RPM? Does the gear blink while the revs are held?", ("handles", "canvas")),
    "W4n": Step("as W4 without --log (does per-call logging over SSH slow the loop?)", "ours",
                ["--draw-parked", "--trace", "pull", "--shift-hold", "2500", "--layout", "split", "--flash-ms", "0",
                 "--palette", "3,4,3,4,3,4,4", "--shift-colour", "4", "--backs", "fill", "--in-flight", "16",
                 "--seconds", "40"], 40, "Video: smoother than W4?", ("handles", "canvas")),
    "W5": Step("pipelined frames (next frame starts once the last is all sent), 32 in flight, revs held 2.5 s per gear", "ours",
               ["--draw-parked", "--trace", "pull", "--shift-hold", "2500", "--layout", "split", "--flash-ms", "0",
                "--palette", "3,4,3,4,3,4,4", "--shift-colour", "4", "--backs", "fill", "--in-flight", "32", "--pipeline",
                "--seconds", "40"], 40, "Video: twice as smooth as W4n? Anything garbled, flicker or a half-drawn frame?",
               ("handles", "canvas")),
    "W5l": Step("as W5 with --log, 15 s (raw lines: answers in order, no strays)", "ours",
                ["--draw-parked", "--trace", "pull", "--shift-hold", "2500", "--layout", "split", "--flash-ms", "0",
                 "--palette", "3,4,3,4,3,4,4", "--shift-colour", "4", "--backs", "fill", "--in-flight", "32", "--pipeline",
                 "--log", "--seconds", "15"], 15, "-", ("handles", "canvas")),
    "W2": Step("as W1, 8 calls in flight", "ours",
               ["--draw-parked", "--trace", "pull", "--layout", "split", "--flash-ms", "0", "--palette", "3,4,3,4,3,4,4",
                "--shift-colour", "4", "--backs", "fill", "--in-flight", "8", "--log", "--seconds", "30"], 30,
               "Video: smoother than W1? Anything missing or garbled?", ("handles", "canvas")),
    "W3": Step("as W1, 16 calls in flight", "ours",
               ["--draw-parked", "--trace", "pull", "--layout", "split", "--flash-ms", "0", "--palette", "3,4,3,4,3,4,4",
                "--shift-colour", "4", "--backs", "fill", "--in-flight", "16", "--log", "--seconds", "30"], 30,
               "Video: as W2?", ("handles", "canvas")),
    # Car run 7 (stories hud-polish/red.md, speed-cameras): is there a red in the palette with Sport selected, and the camera alert.
    "P1": Step("swatch bars fill 0..23 with SPORT selected (does the palette follow the KOMBI design? JayJay3071)", "ours",
               ["--swatches", "0", "--swatch-n", "8", "--swatch-pages", "3", "--show", "both", "--seconds", "8"], 30,
               "Select SPORT first. Photo each page (8 s): page k shows 8k..8k+7 left to right. Is any bar red?",
               ("handles", "canvas")),
    "P1p": Step("as P1 with SPORT+ selected", "ours",
                ["--swatches", "0", "--swatch-n", "8", "--swatch-pages", "3", "--show", "both", "--seconds", "8"], 30,
                "Select SPORT+ first. Photo each page: is any bar red?", ("handles", "canvas")),
    "K1": Step("camera alert, scripted: a camera passed at 90 km/h every 30 s, whole picture then with the bar, 60 s", "ours",
               ["--draw-parked", "--scene", "cam", "--layout", "split", "--flash-ms", "0", "--palette", "3,4,3,4,3,4,4", "--shift-colour", "4", "--backs", "fill", "--in-flight", "32", "--pipeline", "--seconds", "60"], 60,
               "Video: 0-30 s the whole picture (camera, limit sign, shrinking bar) from 4 s to 26 s; 30-60 s the bar with the "
               "badge and the limit 50 in the revs' place from 34 s to 56 s. Readable? Anything garbled?", ("handles", "canvas")),
    "K1r": Step("as K1 with the red from P1 (set red N)", "ours",
                ["--draw-parked", "--scene", "cam", "--layout", "split", "--flash-ms", "0", "--palette", "3,4,3,4,3,4,4", "--shift-colour", "4", "--backs", "fill", "--in-flight", "32", "--pipeline", "--seconds", "60"], 60,
                "Video: is the camera red? The limit and bar still white?", ("handles", "canvas", "red")),
    "K2r": Step("Sport Bar pull with the red: lit 6000+ zone and the blinking gear in red, 40 s", "ours",
                ["--draw-parked", "--trace", "pull", "--shift-hold", "2500", "--layout", "split", "--flash-ms", "0", "--palette", "3,4,3,4,3,4,4", "--shift-colour", "4", "--backs", "fill", "--in-flight", "32", "--pipeline", "--seconds", "40"], 40,
                "Video: the top zone red when lit, the gear red while it blinks past 6400, all else as today?",
                ("handles", "canvas", "red")),
}
SETTABLE = {
    "palette": re.compile(r"(default|\d{1,3}(,\d{1,3}){6})"),
    "canvas": re.compile(r"\d{2,4}x\d{2,4}"),
    "handles": re.compile(r"\d{1,3},\d{1,3}"),
    "inst": re.compile(r"\d{1,2}"),
    "container": re.compile(r"\d{1,3}"),
    "red": re.compile(r"\d{1,3}"),   # the fill index that showed red (P1)
}


def cksum(data):
    """POSIX cksum (what QNX's cksum prints), to check a transfer without sha256 on the unit."""
    crc = 0
    n = len(data)
    tail = bytearray()
    while n:
        tail.append(n & 0xFF)
        n >>= 8
    for byte in bytes(data) + bytes(tail):
        crc ^= byte << 24
        for _ in range(8):
            crc = (crc << 1) ^ 0x04C11DB7 if crc & 0x80000000 else crc << 1
        crc &= 0xFFFFFFFF
    return ~crc & 0xFFFFFFFF


def worker_args(step, state):
    """Our worker's arguments for a step, with the values found earlier in the session."""
    args = list(step.args)
    if "--most-inst" not in args and state.get("inst"):
        args += ["--most-inst", state["inst"]]
    if state.get("handles") and "--handles" not in args:   # run 4 grids name their own pair
        args += ["--handles", state["handles"]]
    if state.get("canvas") and "--probe-sizes" not in args:
        args += ["--canvas", state["canvas"]]
    # Grid steps say it themselves (R4 with, R5 without); swatches and draws need it on 82/02 (run 2).
    if state.get("container") and "--container" not in args and ("--draw-parked" in args or "--swatches" in args):
        args += ["--container", state["container"]]
    if (state.get("palette") not in (None, "default") and "--palette" not in args
            and ("--draw-parked" in args or "--grid" in args)):
        args += ["--palette", state["palette"]]
    if state.get("red") and "--red" not in args and "red" in step.needs:
        args += ["--red", state["red"]]
    return args


def packaged_command(step, folder):
    """The packaged worker has no --seconds: run it in the background and stop it with INT."""
    return (f"cd {folder} && ./hud-shape-qnx {' '.join(step.args)} 2>&1 & p=$!; i=0; "
            f"while kill -0 $p 2>/dev/null && [ $i -lt {step.seconds} ]; do sleep 1; i=$((i+1)); done; "
            "kill -INT $p 2>/dev/null; sleep 3; kill -KILL $p 2>/dev/null; wait $p; echo \"[car-run] rc=$?\"")


def senders(listing):
    return [line.strip() for line in listing.splitlines() if SENDERS.search(line)]


def parse_log(text):
    """Values the runbook's results table asks for, as found in one step's log."""
    found = {}
    m = re.search(r"\[hud\] 82/([0-9A-F]{2}) up, host ([0-9A-F]{4})", text)
    if m:
        found["host"] = f"82/{m.group(1)} host {m.group(2)}"
    if "MOST open failed" in text:
        found["most"] = re.search(r"MOST open failed: -?\d+", text).group(0)
    batches = [(int(n), int(ms)) for n, ms in re.findall(r"batch '[^']*': (\d+) calls answered in (\d+) ms", text)]
    if batches:
        calls, ms = sum(n for n, _ in batches), sum(ms for _, ms in batches)
        found["batches"] = f"{len(batches)} batches, {calls} calls, {ms} ms"
        found["rtt_ms"] = round(ms / calls * 3, 1)   # 3 calls in flight (runbook A3)
    sizes = re.findall(r"\[hud\] rx C04 op ([CD]) len \d+:((?: [0-9a-f]{2})+)", text)
    if sizes:
        found["c04"] = sorted({f"op {op}:{payload}" for op, payload in sizes})
    stats = re.findall(r"errors (\d+) timeouts (\d+) strays \d+, batch (\d+) ms \(max (\d+)\)", text)
    if stats:
        e, t, last, mx = stats[-1]
        found["stats"] = f"errors {e}, timeouts {t}, batch {last} ms (max {mx})"
    # Car run 6: --log stamps tx/rx with t=ms. Each answer pairs with the oldest unanswered call of its function and
    # handle; the spread says whether a wider window keeps the per-call time (latency-bound) or stretches it.
    rtts, waiting = [], {}
    for line in text.splitlines():
        tx = re.match(r"\[hud\] tx ([0-9A-F]{3}) \S+ (\d+) \d+ [0-9a-f]* t=(\d+)$", line)
        rx = re.match(r"\[hud\] rx ([0-9A-F]{3}) op [CD9F] len \d+:((?: [0-9a-f]{2})*) t=(\d+)$", line)
        if tx:
            waiting.setdefault((tx.group(1), int(tx.group(2))), []).append(int(tx.group(3)))
        elif rx:
            b = rx.group(2).split()
            key = (rx.group(1), int(b[0] + b[1], 16) if len(b) >= 2 else 0)
            if waiting.get(key):
                rtts.append(int(rx.group(3)) - waiting[key].pop(0))
    if rtts:
        rtts.sort()
        found["call_ms"] = f"{len(rtts)} calls: median {rtts[len(rtts) // 2]} ms, max {rtts[-1]} ms"
    flags = [w for w in ("refused", "no answer", "profile mismatch", "MOST link lost", "breaks a limit") if w in text]
    # Our worker's summary lines. More rx lines than counted answers means the sender dropped them (car run 1).
    rx = len(re.findall(r"\[hud\] rx ", text))
    counted = [int(n) for n in re.findall(r"\[hud\] [a-z-]+(?: [\d.]+)?: (\d+) answers", text)]
    if rx and counted and rx > sum(counted):
        flags.append("answers dropped")
    if any(int(t) for t in re.findall(r"\[hud\] .*?(\d+) timeouts", text)):
        flags.append("timeouts")
    if flags:
        found["flags"] = flags
    m = re.search(r"\[car-run\] rc=(\d+)", text)
    if m:
        found["rc"] = int(m.group(1))
    return found


class Session:
    def __init__(self, folder, ssh=None, confirm=None, out=print):
        self.dir = folder
        self.path = folder / "session.json"
        self.state = json.loads(self.path.read_text()) if self.path.exists() else {"steps": {}, "notes": {}}
        self.ssh = ssh or self.ssh_run
        self.confirm = confirm or ask
        self.out = out

    def save(self):
        self.dir.mkdir(mode=0o700, parents=True, exist_ok=True)
        self.path.write_text(json.dumps(self.state, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    def ssh_run(self, command, stdin=None, timeout=60, stream=False):
        args = ["ssh", "-T", "-o", "StrictHostKeyChecking=yes", "-o", "ConnectTimeout=5"]
        if self.state.get("legacy"):
            for option in LEGACY:
                args += ["-o", option]
        args += ["root@" + self.state["head"], command]
        if not stream:
            r = subprocess.run(args, input=stdin, capture_output=True, timeout=timeout)
            return r.returncode, (r.stdout + r.stderr).decode(errors="replace")
        # Killing ssh does not stop the remote worker (no tty), so the caller aborts on None.
        lines = []
        with subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                              start_new_session=True) as p:
            def kill():
                try:
                    os.killpg(p.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            timer = threading.Timer(timeout, kill)
            timer.start()
            try:
                for line in p.stdout:
                    lines.append(line)
                    self.out(line.rstrip("\n"))
                rc = p.wait()
            except KeyboardInterrupt:
                kill()
                return None, "".join(lines) + "\n[car-run] Ctrl-C: ssh killed\n"
            finally:
                expired = not timer.is_alive()
                timer.cancel()
            if expired:
                return None, "".join(lines) + f"\n[car-run] TIMEOUT after {timeout} s: ssh killed\n"
            return rc, "".join(lines)

    def hmi(self, command):
        """A command on the node that runs the HMI and DSYSMOSTServer."""
        if self.state.get("runner") == "omap":
            return "on -f hu-omap sh -c " + shlex.quote(OMAP_ENV + command)
        return command

    def run_dir(self):
        return f"/net/{self.state['node']}{STAGE}" if self.state.get("runner") == "omap" else STAGE

    def log(self, step_id, command, rc, text):
        logs = self.dir / "logs"
        logs.mkdir(mode=0o700, parents=True, exist_ok=True)
        n = sum(1 for _ in logs.glob(step_id + "-*.log")) + 1
        name = f"{step_id}-{n}.log"
        stamp = datetime.datetime.now().isoformat(timespec="seconds")
        (logs / name).write_text(f"# {stamp} rc={rc}\n# {command}\n{text}", encoding="utf-8")
        self.state["steps"][step_id] = {"rc": rc, "at": stamp, "log": "logs/" + name, "found": parse_log(text)}
        self.save()
        return name

    def gate(self, prompt):
        if not self.confirm(prompt):
            raise SystemExit("not confirmed, nothing sent")

    def inventory(self):
        rc, uname = self.ssh("uname -a")
        if rc or "QNX" not in uname or "armle" not in uname:
            raise SystemExit(f"not an ARM QNX head unit: {uname.strip()}")
        node = uname.split()[1]
        self.state["node"] = node
        split = "jacinto" in node
        _, local = self.ssh("pidin arg")
        omap = ""
        if split:
            _, omap = self.ssh("on -f hu-omap pidin arg")
        if "DSYSMOSTServer" in local:
            self.state["runner"] = "local"
        elif "DSYSMOSTServer" in omap:
            self.state["runner"] = "omap"
        else:
            self.state["runner"] = None
        _, plugins = self.ssh(f"on -f hu-omap ls {PLUGINS}" if split else f"ls {PLUGINS}")
        _, space = self.ssh("df -k /tmp")
        near = [line.strip() for line in (local + "\n" + omap).splitlines() if NEARBY.search(line)]
        text = (f"uname: {uname.strip()}\nsplit-node: {split}\nMOST server on: {self.state['runner'] or 'NOT FOUND'}\n"
                f"plugins:\n{plugins}\nnearby processes:\n" + "\n".join(near) + f"\n/tmp:\n{space}\n")
        self.state["senders"] = senders(local + "\n" + omap)
        # What /tmp held before we touched anything: cleanup compares against it.
        self.state.setdefault("baseline", self.tmp_listing())
        self.out(text)
        self.log("A2", "inventory", 0, text)
        if not self.state["runner"]:
            self.out("SURPRISE: DSYSMOSTServer not found on any node; worker steps are blocked.")
        if self.state["senders"]:
            self.out("BLOCK: another HUD sender runs: " + "; ".join(self.state["senders"]))

    def tmp_listing(self):
        listing = {"local": sorted(self.ssh("ls -a /tmp")[1].split())}
        if "jacinto" in self.state.get("node", ""):
            listing["omap"] = sorted(self.ssh("on -f hu-omap ls -a /tmp")[1].split())
        return listing

    def check_clear(self):
        """Right before a worker step: the MOST server is known and nothing else draws on the HUD."""
        if not self.state.get("runner"):
            raise SystemExit("run A2 first (or DSYSMOSTServer was not found)")
        listing = self.ssh("pidin arg")[1]
        if "jacinto" in self.state.get("node", ""):
            listing += self.ssh("on -f hu-omap pidin arg")[1]
        found = senders(listing)
        if found:
            raise SystemExit("another HUD sender runs, untick it in iDrive first: " + "; ".join(found))

    def stage(self, files):
        self.gate(f"Copy {', '.join(files)} to {STAGE} on the head unit (temporary, nothing installed)?")
        self.ssh(f"mkdir -p {STAGE}")
        staged = self.state.setdefault("staged", {})
        for name, data in files.items():
            # Some firmware (e.g. X20511I) has no chmod, so the worker can't be made
            # executable the usual way. Seed the file from /bin/cat (mode 0555), then
            # overwrite its contents as root without changing the mode: the ELF stays
            # executable. rm -f first so the mode is reseeded on a re-stage.
            self.ssh(f"rm -f {STAGE}/{name}")
            rc_cp, _ = self.ssh(f"cp /bin/cat {STAGE}/{name}")
            rc, _ = self.ssh(f"cat > {STAGE}/{name}", stdin=data, timeout=120)
            if rc_cp or rc:
                raise SystemExit(f"{name}: copy failed (cp rc {rc_cp}, cat rc {rc})")
            rc2, answer = self.ssh(f"cksum {STAGE}/{name} 2>/dev/null")
            want = f"{cksum(data)} {len(data)}"
            if rc2 == 0 and answer.strip():
                if not answer.startswith(want):
                    raise SystemExit(f"{name}: transfer check failed, want '{want}', got '{answer.strip()}'")
                staged[name] = hashlib.sha256(data).hexdigest()
                self.out(f"staged {name}: {len(data)} bytes, cksum {want.split()[0]} matches")
            else:
                # Trimmed QNX userland (e.g. firmware X20511I) has no cksum; the only
                # check available is the byte size via ls, over the reliable SSH channel.
                rc3, listing = self.ssh(f"ls -l {STAGE}/{name}")
                fields = listing.split()
                size = int(fields[4]) if rc3 == 0 and len(fields) > 4 and fields[4].isdigit() else None
                if size != len(data):
                    raise SystemExit(f"{name}: size check failed, want {len(data)}, got {size!r} ({listing.strip()})")
                staged[name] = hashlib.sha256(data).hexdigest()
                self.out(f"staged {name}: {len(data)} bytes, size matches (no cksum on unit)")
        self.save()

    def run(self, step_id):
        step = STEPS[step_id]
        if step.kind == "capture":
            out = self.dir / "capture"
            cmd = [sys.executable, str(Path(__file__).with_name("capture_headunit.py")), "--head", self.state["head"],
                   "--out", str(out)] + (["--legacy"] if self.state.get("legacy") else [])
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
            self.out(r.stdout + r.stderr)
            self.log(step_id, " ".join(cmd), r.returncode, r.stdout + r.stderr)
            return
        if step.kind == "inventory":
            self.inventory()
            return
        missing = [k for k in step.needs if not self.state.get(k)]
        if missing:
            raise SystemExit(f"{step_id} needs `set {' / set '.join(missing)}` first (see the runbook)")
        binary = "hud-shape-qnx" if step.kind == "packaged" else "hud-sportbar-qnx"
        if binary not in self.state.get("staged", {}):
            raise SystemExit(f"stage {'packaged' if step.kind == 'packaged' else 'ours'} first")
        self.check_clear()
        if step.kind == "packaged":
            command = self.hmi(packaged_command(step, self.run_dir()))
        else:
            args = " ".join(shlex.quote(a) for a in worker_args(step, self.state))
            command = self.hmi(f"cd {self.run_dir()} && ./{binary} {args} 2>&1; echo \"[car-run] rc=$?\"")
        self.out(f"{step_id}: {step.title}\n  {command}")
        if step.film:
            self.out("  FILM: " + step.film)
        if not step.readonly:
            self.gate(f"{step_id}: parked, owner present, ready? Runs at most {step.seconds} s.")
        rc, text = self.ssh(command, timeout=step.seconds + 45, stream=True)
        if rc is None:
            self.abort()
        name = self.log(step_id, command, rc, text)
        found = self.state["steps"][step_id]["found"]
        bad = rc is None or found.get("rc", rc) != 0 or "flags" in found or "most" in found
        self.out(("SURPRISE: stop and look. " if bad else "ok. ") + json.dumps(found, ensure_ascii=False) + f"  ({name})")

    def abort(self):
        """Stop our workers (they clear on TERM), then one 1 s run so a stuck image goes away."""
        kill = "slay -f -s TERM hud-sportbar-qnx 2>/dev/null; slay -f -s INT hud-shape-qnx 2>/dev/null; sleep 2; true"
        self.ssh(self.hmi(kill), timeout=30)
        if "hud-sportbar-qnx" in self.state.get("staged", {}) and self.state.get("runner"):
            clear = f"cd {self.run_dir()} && ./hud-sportbar-qnx --draw-parked --seconds 1 2>&1"
            rc, text = self.ssh(self.hmi(clear), timeout=45)
            self.log("abort", clear, rc, text)
        self.out("aborted: workers stopped, HUD cleared once")

    def cleanup(self):
        self.gate(f"Stop workers and delete {STAGE} on the head unit?")
        self.ssh(self.hmi("slay -f -s TERM hud-sportbar-qnx 2>/dev/null; slay -f -s INT hud-shape-qnx 2>/dev/null; true"))
        self.ssh(f"rm -rf {STAGE}")
        rc, left = self.ssh(f"ls {STAGE}")
        if rc == 0:
            raise SystemExit(f"{STAGE} still there: {left.strip()}")
        self.state["staged"] = {}
        self.save()
        self.out(f"{STAGE} removed")
        # Nothing of ours may stay behind: no worker process, no new entry in /tmp on either node.
        listing = self.ssh("pidin arg")[1]
        if "jacinto" in self.state.get("node", ""):
            listing += self.ssh("on -f hu-omap pidin arg")[1]
        running = [line.strip() for line in listing.splitlines() if "hud-sportbar-qnx" in line or "hud-shape-qnx" in line]
        before, after = self.state.get("baseline"), self.tmp_listing()
        new = {node: sorted(set(names) - set(before.get(node, []))) for node, names in after.items()} if before else {}
        new = {node: names for node, names in new.items() if names}
        text = (f"workers still running: {running or 'none'}\n/tmp now: {after}\n"
                f"baseline: {before or 'NOT RECORDED (run A2 before stage next time)'}\nnew since baseline: {new or 'none'}\n")
        self.log("cleanup", "cleanup check", 0, text)
        if running or new or not before:
            raise SystemExit("LEFTOVER, look before you leave the car:\n" + text)
        self.out("clean: no workers, /tmp matches the A2 baseline")

    def report(self):
        steps, notes = self.state["steps"], self.state.get("notes", {})
        rows = []
        extra = [(k, Step("abort" if k == "abort" else "owner's check", "")) for k in {**steps, **notes} if k not in STEPS]
        for step_id, step in list(STEPS.items()) + extra:
            if step_id not in steps and step_id not in notes:
                continue
            s = steps.get(step_id, {})
            found = "; ".join(f"{k}: {v}" for k, v in s.get("found", {}).items() if k != "rc")
            note = " / ".join(notes.get(step_id, []))
            rows.append(f"| {step_id} | {step.title} | {s.get('rc', '')} | {found} | {note} | {s.get('log', '')} |")
        a2 = steps.get("A2", {})
        text = (f"# HUD car run, {self.state.get('head', '')}\n\n"
                f"- Node: {self.state.get('node', '?')}, MOST server on: {self.state.get('runner', '?')}\n"
                f"- Set: " + ", ".join(f"{k}={self.state[k]}" for k in SETTABLE if self.state.get(k)) + "\n"
                f"- Staged: " + ", ".join(f"{k} {v[:12]}" for k, v in self.state.get("staged", {}).items()) + "\n"
                f"- A2 log: {a2.get('log', 'not run')}\n\n"
                "| Step | What | rc | Found in the log | Owner's notes | Log |\n|---|---|---|---|---|---|\n"
                + "\n".join(rows) + "\n")
        (self.dir / "results.md").write_text(text, encoding="utf-8")
        self.out(text)


def ask(prompt):
    if not sys.stdin.isatty():
        return False
    return input(prompt + " [y/N] ").strip().lower() == "y"


def load_files(which, package=None):
    if which == "ours":
        data = OURS.read_bytes()
        if not is_arm_elf(data):
            raise SystemExit(f"{OURS} is not an ARM ELF; run native/docker/qnx.sh build")
        return {"hud-sportbar-qnx": data}
    pkg = Path(package) if package else (Path(os.environ["HUD_GAUGES_ZIP"]) if os.environ.get("HUD_GAUGES_ZIP") else None)
    if pkg is None or not pkg.is_file():
        raise SystemExit("pass --package PATH or set HUD_GAUGES_ZIP to stage an external reference hud-gauges.zip")
    with zipfile.ZipFile(pkg) as z:
        sums = dict(reversed(line.split()) for line in z.read("SHA256SUMS").decode().splitlines() if line.strip())
        files = {}
        for name in PACKAGED:
            data = z.read(name)
            if hashlib.sha256(data).hexdigest() != sums[name]:
                raise SystemExit(f"{name}: sha256 differs from the package's SHA256SUMS")
            files[name.split("/")[-1]] = data
    return files


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--session", type=Path, default=Path.home() / f"nbt-hud-run-{datetime.date.today()}",
                   help="private folder for logs and results (outside the repo)")
    p.add_argument("--head", type=enet_ip, help="head unit ENET IP (first call only, kept in the session)")
    p.add_argument("--legacy", action="store_true", help="allow legacy SSH algorithms for this ENET connection")
    p.add_argument("--confirmed", action="store_true", help="the owner confirmed this step in chat")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("plan", help="list the steps and what has run")
    s = sub.add_parser("stage", help="copy a worker to /tmp/hud-run and check it")
    s.add_argument("which", choices=("ours", "packaged"))
    s.add_argument("--package", type=Path, default=None,
                   help="path to an external reference hud-gauges.zip (or set HUD_GAUGES_ZIP)")
    r = sub.add_parser("run", help="run one runbook step")
    r.add_argument("step", choices=list(STEPS))
    s = sub.add_parser("set", help="keep a value found in an earlier step")
    s.add_argument("key", choices=list(SETTABLE))
    s.add_argument("value")
    n = sub.add_parser("note", help="record what the owner saw")
    n.add_argument("step")
    n.add_argument("text")
    sub.add_parser("abort", help="stop the workers now and clear the HUD (no confirmation)")
    sub.add_parser("cleanup", help="stop workers and delete /tmp/hud-run")
    sub.add_parser("report", help="write results.md from the logs")
    args = p.parse_args()

    session = Session(args.session, confirm=(lambda _prompt: True) if args.confirmed else None)
    if args.head:
        session.state["head"] = args.head
        session.state["legacy"] = args.legacy
        session.save()
    if args.cmd not in ("plan", "set", "note", "report") and not session.state.get("head"):
        raise SystemExit("pass --head 169.254.x.x on the first call")
    try:
        if args.cmd == "plan":
            for step_id, step in STEPS.items():
                done = session.state["steps"].get(step_id)
                mark = f"rc={done['rc']} {done['log']}" if done else "-"
                print(f"{step_id:5} {step.title:50} {mark}")
        elif args.cmd == "stage":
            session.stage(load_files(args.which, package=args.package))
        elif args.cmd == "run":
            session.run(args.step)
        elif args.cmd == "set":
            if not SETTABLE[args.key].fullmatch(args.value):
                raise SystemExit(f"bad {args.key}: {args.value}")
            session.state[args.key] = args.value
            session.save()
        elif args.cmd == "note":
            session.state.setdefault("notes", {}).setdefault(args.step, []).append(args.text)
            session.save()
        elif args.cmd == "abort":
            session.abort()
        elif args.cmd == "cleanup":
            session.cleanup()
        else:
            session.report()
    except (OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        raise SystemExit(f"{type(error).__name__}: {error}") from None


if __name__ == "__main__":
    main()
