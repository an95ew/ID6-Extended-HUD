"""Generates src/hud/hud_glyphs.h: the manoeuvre pictures as HUD polygons, from Mapbox's turn icons.

The source is ManeuversStyleKit.swift of mapbox-navigation-ios v0.40.0 (ISC licence, drawings from Mapbox's CC0
Maneuvers.pcvd). It is downloaded at its pinned tag and checked by hash; it is not kept in this repo. This script runs
the PaintCode drawing code it contains: transforms, Bezier paths, fills, strokes and the roundabout's dash. Then it
turns every fill into a polygon and every stroke into the outline of the stroke, so each piece is one ImageAddPolygon
call. The HUD fills whole (concave) polygons, so no convex split is needed.

    python3 tools/hud_glyphs_gen.py                        # downloads, writes src/hud/hud_glyphs.h
    python3 tools/hud_glyphs_gen.py --swift FILE --json OUT # a local copy; OUT: the shapes for a preview

Units in the header: 1/64 of Mapbox's 32-unit canvas, y down. Tone 0 is the route (primary colour), 1 the context
(secondary, grey), 2 a hole cut by the even-odd rule (drawn on top in the context tone).
"""
import argparse
import ast
import hashlib
import json
import math
import os
import re
import sys
import urllib.request

URL = "https://raw.githubusercontent.com/mapbox/mapbox-navigation-ios/v0.40.0/MapboxNavigation/ManeuversStyleKit.swift"
SHA256 = "af82189c1c495035e8df02d4f3bd64a554212fa8eae1e6cffa8a3dbe043f0887"
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "src", "hud", "hud_glyphs.h")

GLYPHS = ["Arrowstraight", "Arrowright", "Arrowslightright", "Arrowsharpright", "Arrow180right", "Arrive",
          "Arriveright", "Starting", "Destination", "Merge", "Fork", "Offramp"]
ROUNDABOUT_ANGLES = list(range(15, 360, 15))   # the exit angle, 90 = first right
UNIT = 64           # header units per Mapbox unit
TOL = 0.05          # curve flattening, Mapbox units (a 32-unit picture is about 80 HUD pixels: 0.12 px)
EPS = 0.06          # outline simplification
MAX_PTS = 99        # ImageAddPolygon

NOTICE = """Copyright © 2014–2020, Mapbox

Permission to use, copy, modify, and/or distribute this software for any purpose with or without fee is hereby
granted, provided that the above copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF
THIS SOFTWARE."""


# ---- geometry ----

def mat_mul(a, b):   # affine (a, b, c, d, tx, ty): x' = a x + c y + tx, y' = b x + d y + ty; a then b
    return (a[0] * b[0] + a[1] * b[2], a[0] * b[1] + a[1] * b[3], a[2] * b[0] + a[3] * b[2],
            a[2] * b[1] + a[3] * b[3], a[4] * b[0] + a[5] * b[2] + b[4], a[4] * b[1] + a[5] * b[3] + b[5])


def apply(m, p):
    return (m[0] * p[0] + m[2] * p[1] + m[4], m[1] * p[0] + m[3] * p[1] + m[5])


def cubic(p0, p1, p2, p3, out, depth=0):
    """Adaptive flattening: split until the control points lie within TOL of the chord."""
    def dist(p, a, b):
        dx, dy = b[0] - a[0], b[1] - a[1]
        n = math.hypot(dx, dy)
        if n < 1e-12:
            return math.hypot(p[0] - a[0], p[1] - a[1])
        return abs((p[0] - a[0]) * dy - (p[1] - a[1]) * dx) / n
    if depth > 12 or max(dist(p1, p0, p3), dist(p2, p0, p3)) <= TOL:
        out.append(p3)
        return
    m = lambda a, b: ((a[0] + b[0]) / 2, (a[1] + b[1]) / 2)
    p01, p12, p23 = m(p0, p1), m(p1, p2), m(p2, p3)
    p012, p123 = m(p01, p12), m(p12, p23)
    mid = m(p012, p123)
    cubic(p0, p01, p012, mid, out, depth + 1)
    cubic(mid, p123, p23, p3, out, depth + 1)


def simplify(pts, eps):
    """Douglas-Peucker on an open polyline."""
    if len(pts) < 3:
        return pts
    a, b = pts[0], pts[-1]
    dx, dy = b[0] - a[0], b[1] - a[1]
    n = math.hypot(dx, dy)
    best, idx = -1, 0
    for i in range(1, len(pts) - 1):
        p = pts[i]
        d = abs((p[0] - a[0]) * dy - (p[1] - a[1]) * dx) / n if n > 1e-12 else math.hypot(p[0] - a[0], p[1] - a[1])
        if d > best:
            best, idx = d, i
    if best <= eps:
        return [a, b]
    return simplify(pts[:idx + 1], eps)[:-1] + simplify(pts[idx:], eps)


def simplify_closed(pts, eps):
    pts = dedupe(pts)
    if len(pts) < 4:
        return pts
    far = max(range(len(pts)), key=lambda i: math.hypot(pts[i][0] - pts[0][0], pts[i][1] - pts[0][1]))
    a = simplify(pts[:far + 1], eps)
    b = simplify(pts[far:] + [pts[0]], eps)
    return dedupe(a[:-1] + b[:-1])


def dedupe(pts):
    out = []
    for p in pts:
        if not out or math.hypot(p[0] - out[-1][0], p[1] - out[-1][1]) > 1e-6:
            out.append(p)
    if len(out) > 1 and math.hypot(out[0][0] - out[-1][0], out[0][1] - out[-1][1]) <= 1e-6:
        out.pop()
    return out


def area(pts):
    return sum(pts[i][0] * pts[(i + 1) % len(pts)][1] - pts[(i + 1) % len(pts)][0] * pts[i][1]
               for i in range(len(pts))) / 2


def inside(p, poly):
    """Ray casting."""
    c = False
    for i in range(len(poly)):
        a, b = poly[i], poly[i - 1]
        if (a[1] > p[1]) != (b[1] > p[1]) and p[0] < (b[0] - a[0]) * (p[1] - a[1]) / (b[1] - a[1]) + a[0]:
            c = not c
    return c


def crosses(a, b, c, d):
    def o(p, q, r):
        v = (q[0] - p[0]) * (r[1] - p[1]) - (q[1] - p[1]) * (r[0] - p[0])
        return 0 if abs(v) < 1e-9 else (1 if v > 0 else -1)
    return o(a, b, c) * o(a, b, d) < 0 and o(c, d, a) * o(c, d, b) < 0


def simple(pts):
    n = len(pts)
    for i in range(n):
        for j in range(i + 2, n):
            if i == 0 and j == n - 1:
                continue
            if crosses(pts[i], pts[(i + 1) % n], pts[j], pts[(j + 1) % n]):
                return False
    return True


def trim(pts, length):
    """The first `length` units of a polyline (a dash of [length, long gap])."""
    out, left = [pts[0]], length
    for a, b in zip(pts, pts[1:]):
        d = math.hypot(b[0] - a[0], b[1] - a[1])
        if d >= left:
            t = left / d if d else 0
            out.append((a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t))
            return out
        out.append(b)
        left -= d
    return out


def unloop(pts):
    """Cuts the small loops an inner offset makes where a curve is tighter than half the stroke width."""
    def hit(a, b, c, d):
        den = (b[0] - a[0]) * (d[1] - c[1]) - (b[1] - a[1]) * (d[0] - c[0])
        t = ((c[0] - a[0]) * (d[1] - c[1]) - (c[1] - a[1]) * (d[0] - c[0])) / den
        return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)
    changed = True
    while changed:
        changed = False
        for i in range(len(pts) - 1):
            for j in range(len(pts) - 2, i + 1, -1):
                if crosses(pts[i], pts[i + 1], pts[j], pts[j + 1]):
                    pts = pts[:i + 1] + [hit(pts[i], pts[i + 1], pts[j], pts[j + 1])] + pts[j + 1:]
                    changed = True
                    break
            if changed:
                break
    return pts


def stroke(pts, w, closed, round_join):
    """Outline of a stroke with butt caps: one polygon per open polyline. A closed one is split in two halves."""
    if closed:
        pts = pts + [pts[0]]
        h = len(pts) // 2
        return stroke(pts[:h + 1], w, False, round_join) + stroke(pts[h:], w, False, round_join)
    pts = dedupe(pts)
    if len(pts) < 2:
        return []
    r = w / 2
    left, right = [], []
    for i, p in enumerate(pts):
        ns = []
        for j in (i - 1, i):
            if 0 <= j < len(pts) - 1:
                dx, dy = pts[j + 1][0] - pts[j][0], pts[j + 1][1] - pts[j][1]
                n = math.hypot(dx, dy)
                ns.append((-dy / n, dx / n))
        if len(ns) == 1:
            nx, ny = ns[0]
            left.append((p[0] + nx * r, p[1] + ny * r))
            right.append((p[0] - nx * r, p[1] - ny * r))
            continue
        (ax, ay), (bx, by) = ns
        mx, my = ax + bx, ay + by
        mn = math.hypot(mx, my)
        cos_half = mn / 2
        if mn < 1e-9 or cos_half < 0.3:   # a cusp: square it off
            left += [(p[0] + ax * r, p[1] + ay * r), (p[0] + bx * r, p[1] + by * r)]
            right += [(p[0] - ax * r, p[1] - ay * r), (p[0] - bx * r, p[1] - by * r)]
            continue
        mx, my = mx / mn, my / mn
        turn = ax * by - ay * bx   # > 0: the left side is outside
        miter = (p[0] + mx * r / cos_half, p[1] + my * r / cos_half)
        miter_r = (p[0] - mx * r / cos_half, p[1] - my * r / cos_half)
        def arc(sgn, a0, a1):
            t0, t1 = math.atan2(sgn * a0[1], sgn * a0[0]), math.atan2(sgn * a1[1], sgn * a1[0])
            while t1 - t0 > math.pi:
                t1 -= 2 * math.pi
            while t0 - t1 > math.pi:
                t1 += 2 * math.pi
            k = max(1, int(abs(t1 - t0) / (math.pi / 8)))
            return [(p[0] + r * math.cos(t0 + (t1 - t0) * s / k), p[1] + r * math.sin(t0 + (t1 - t0) * s / k))
                    for s in range(k + 1)]
        if turn > 0:
            left += arc(1, (ax, ay), (bx, by)) if round_join else [miter]
            right.append(miter_r)
        else:
            left.append(miter)
            right += arc(-1, (ax, ay), (bx, by)) if round_join else [miter_r]
    return [unloop(left) + unloop(right)[::-1]]


# ---- the Swift subset PaintCode writes ----

class Rect:
    def __init__(self, x, y, w, h):
        self.x, self.y, self.width, self.height = x, y, w, h
        self.minX, self.minY, self.maxX, self.maxY = x, y, x + w, y + h


class NS:
    def __init__(self, **kw):
        self.__dict__.update(kw)


class Path:
    def __init__(self):
        self.subs, self.cur = [], None   # [(points, closed)]
        self.width, self.even_odd, self.round_join = 1.0, False, False

    def move(self, p):
        self.cur = [p]
        self.subs.append([self.cur, False])

    def line(self, p):
        self.cur.append(p)

    def curve(self, p, c1, c2):
        cubic(self.cur[-1], c1, c2, p, self.cur)

    def close(self):
        self.subs[-1][1] = True


def swift_expr(e):
    e = e.strip()
    m = re.fullmatch(r"(\w+) \? (.+?) : (.+)", e)
    if m:
        e = f"(({m.group(2)}) if {m.group(1)} else ({m.group(3)}))"
    e = e.replace("CGFloat.pi", "pi").replace("CGRect(", "Rect(").replace("CGPoint(", "(").replace("CGSize(", "Size(")
    e = re.sub(r"(\(|, )(\w+): ", r"\1", e)
    return e


def safe_eval(e, env):
    """Arithmetic only: numbers, names and attributes from env, + - * /, unary minus, calls to the functions env
    provides, tuples and a conditional. Nothing from the Swift file can run as code."""
    ops = {ast.Add: lambda a, b: a + b, ast.Sub: lambda a, b: a - b, ast.Mult: lambda a, b: a * b,
           ast.Div: lambda a, b: a / b}

    def ev(n):
        if isinstance(n, ast.Expression):
            return ev(n.body)
        if isinstance(n, ast.Constant) and isinstance(n.value, (int, float, bool)):
            return n.value
        if isinstance(n, ast.Name) and n.id in env:
            return env[n.id]
        if isinstance(n, ast.Attribute) and not n.attr.startswith("_"):
            return getattr(ev(n.value), n.attr)
        if isinstance(n, ast.BinOp) and type(n.op) in ops:
            return ops[type(n.op)](ev(n.left), ev(n.right))
        if isinstance(n, ast.UnaryOp) and isinstance(n.op, (ast.USub, ast.UAdd)):
            v = ev(n.operand)
            return -v if isinstance(n.op, ast.USub) else v
        if isinstance(n, ast.Tuple):
            return tuple(ev(x) for x in n.elts)
        if isinstance(n, ast.IfExp):
            return ev(n.body) if ev(n.test) else ev(n.orelse)
        if isinstance(n, ast.Call) and isinstance(n.func, ast.Name) and n.func.id in env and not n.keywords:
            f = env[n.func.id]
            if callable(f):
                return f(*[ev(x) for x in n.args])
        raise SystemExit(f"unsupported expression: {e}")
    return ev(ast.parse(swift_expr(e), mode="eval"))


def run(body, params):
    env = dict(params)
    env.update(pi=math.pi, sin=math.sin, cos=math.cos, min=min, max=max, abs=abs, Rect=Rect,
               Size=lambda w, h: NS(width=w, height=h), true=True, false=False)
    ev = lambda e: safe_eval(e, env)
    ctm, stack, dash = (1, 0, 0, 1, 0, 0), [], None
    fill_tone = stroke_tone = 0
    shapes = []   # (tone, points in Mapbox units)

    def emit_fill(path):
        """A sub-path inside another one of the same fill is a hole: always with the even-odd rule, and with the
        non-zero rule when it winds the other way (the destination pin). Holes are drawn on top in the context tone."""
        mine = []
        for pts, _closed in path.subs:
            pts = simplify_closed([apply(ctm, p) for p in pts], EPS)
            if len(pts) >= 3:
                mine.append([fill_tone, pts])
        for s_ in mine:
            for o in mine:
                if o is not s_ and inside(s_[1][0], o[1]) and abs(area(o[1])) > abs(area(s_[1])) and \
                        (path.even_odd or (area(o[1]) > 0) != (area(s_[1]) > 0)):
                    s_[0] = 2
        shapes.extend(sorted(mine, key=lambda x: x[0] == 2))

    def emit_stroke(path):
        k = math.sqrt(abs(ctm[0] * ctm[3] - ctm[1] * ctm[2]))
        for pts, closed in path.subs:
            pts = [apply(ctm, p) for p in pts]
            if dash is not None:
                pts = trim(pts + ([pts[0]] if closed else []), dash * k)
                closed = False
            for outline in stroke(simplify(dedupe(pts) + ([] if closed else []), EPS / 2) if not closed else dedupe(pts),
                                  path.width * k, closed, path.round_join):
                outline = simplify_closed(outline, EPS)
                if len(outline) >= 3:
                    shapes.append([stroke_tone, outline])

    for raw in body:
        s = raw.strip()
        if not s or s.startswith("//") or s in ("}", "{") or s.startswith("let context ="):
            continue
        if m := re.fullmatch(r"let resizedFrame: CGRect = resizing\.apply\(rect: (.+), target: targetFrame\)", s):
            env["resizedFrame"] = ev(m.group(1))
        elif m := re.fullmatch(r"let (\w+) = UIBezierPath\(\)", s):
            env[m.group(1)] = Path()
        elif m := re.fullmatch(r"let (\w+) = UIBezierPath\(rect: (.+)\)", s):
            r, p = ev(m.group(2)), Path()
            p.move((r.minX, r.minY)); p.line((r.maxX, r.minY)); p.line((r.maxX, r.maxY)); p.line((r.minX, r.maxY))
            p.close()
            env[m.group(1)] = p
        elif m := re.fullmatch(r"let (\w+) = UIBezierPath\(ovalIn: (.+)\)", s):
            r, p = ev(m.group(2)), Path()
            k = 48
            p.move((r.minX + r.width, r.minY + r.height / 2))
            for i in range(1, k):
                a = 2 * math.pi * i / k
                p.line((r.minX + r.width / 2 * (1 + math.cos(a)), r.minY + r.height / 2 * (1 + math.sin(a))))
            p.close()
            env[m.group(1)] = p
        elif m := re.fullmatch(r"let (\w+) = UIBezierPath\(roundedRect: (.+), byRoundingCorners: \[(.*)\], cornerRadii: (.+)\)", s):
            r, cr, p = ev(m.group(2)), ev(m.group(4)), Path()
            tl, tr = ".topLeft" in m.group(3), ".topRight" in m.group(3)
            rad = min(cr.width, r.width / 2, r.height / 2)
            pts = []
            for on, cx, cy, a0, corner in ((tl, r.minX + rad, r.minY + rad, 180, (r.minX, r.minY)),
                                           (tr, r.maxX - rad, r.minY + rad, 270, (r.maxX, r.minY))):
                if not on:
                    pts.append(corner)
                    continue
                for i in range(7):
                    a = math.radians(a0 + 90 * i / 6)
                    pts.append((cx + rad * math.cos(a), cy + rad * math.sin(a)))
            pts += [(r.maxX, r.maxY), (r.minX, r.maxY)]
            p.move(pts[0])
            for q in pts[1:]:
                p.line(q)
            p.close()
            env[m.group(1)] = p
        elif m := re.fullmatch(r"let (\w+)(?:: \w+)? = (.+)", s):
            env[m.group(1)] = ev(m.group(2))
        elif m := re.fullmatch(r"var (\w+) = .+", s):
            pass
        elif s == "context.saveGState()":
            stack.append((ctm, dash))
        elif s == "context.restoreGState()":
            ctm, dash = stack.pop()
        elif m := re.fullmatch(r"context\.translateBy\(x: (.+), y: (.+)\)", s):
            ctm = mat_mul((1, 0, 0, 1, ev(m.group(1)), ev(m.group(2))), ctm)
        elif m := re.fullmatch(r"context\.scaleBy\(x: (.+), y: (.+)\)", s):
            ctm = mat_mul((ev(m.group(1)), 0, 0, ev(m.group(2)), 0, 0), ctm)
        elif m := re.fullmatch(r"context\.rotate\(by: (.+)\)", s):
            a = ev(m.group(1))
            ctm = mat_mul((math.cos(a), math.sin(a), -math.sin(a), math.cos(a), 0, 0), ctm)
        elif m := re.fullmatch(r"context\.setLineDash\(phase: 0, lengths: \[(.+), (.+)\]\)", s):
            dash = ev(m.group(1))
        elif m := re.fullmatch(r"(\w+)\.move\(to: (.+)\)", s):
            env[m.group(1)].move(ev(m.group(2)))
        elif m := re.fullmatch(r"(\w+)\.addLine\(to: (.+)\)", s):
            env[m.group(1)].line(ev(m.group(2)))
        elif m := re.fullmatch(r"(\w+)\.addCurve\(to: (.+), controlPoint1: (.+), controlPoint2: (.+)\)", s):
            env[m.group(1)].curve(ev(m.group(2)), ev(m.group(3)), ev(m.group(4)))
        elif m := re.fullmatch(r"(\w+)\.close\(\)", s):
            env[m.group(1)].close()
        elif m := re.fullmatch(r"(\w+)\.lineWidth = (.+)", s):
            env[m.group(1)].width = ev(m.group(2))
        elif m := re.fullmatch(r"(\w+)\.lineJoinStyle = \.(\w+)", s):
            env[m.group(1)].round_join = m.group(2) == "round"
        elif m := re.fullmatch(r"(\w+)\.lineCapStyle = \.(\w+)", s):
            if m.group(2) != "butt":
                raise SystemExit(f"line cap {m.group(2)} not supported")
        elif m := re.fullmatch(r"(\w+)\.usesEvenOddFillRule = true", s):
            env[m.group(1)].even_odd = True
        elif m := re.fullmatch(r"(primary|secondary)Color\.set(Fill|Stroke)\(\)", s):
            if m.group(2) == "Fill":
                fill_tone = 0 if m.group(1) == "primary" else 1
            else:
                stroke_tone = 0 if m.group(1) == "primary" else 1
        elif m := re.fullmatch(r"(\w+)\.fill\(\)", s):
            emit_fill(env[m.group(1)])
        elif m := re.fullmatch(r"(\w+)\.stroke\(\)", s):
            emit_stroke(env[m.group(1)])
        else:
            raise SystemExit(f"unsupported Swift line: {s}")
    return shapes


def functions(src):
    out, name, body = {}, None, []
    for line in src.splitlines():
        m = re.search(r"class func draw(\w+)\(", line)
        if m:
            if name:
                out[name] = body
            name, body = m.group(1), []
        elif name and ("//// Generated Images" in line or "public enum ResizingBehavior" in line):
            out[name], name = body, None
        elif name:
            body.append(line)
    if name:
        out[name] = body
    return out


def glyph(funcs, name, **extra):
    params = dict(size=NS(width=32, height=32), flipHorizontally=False, roundabout_angle=90, roundabout_radius=6.5)
    params.update(extra)
    shapes = run(funcs[name], params)
    for tone, pts in shapes:
        if len(pts) > MAX_PTS:
            raise SystemExit(f"{name}: a polygon has {len(pts)} points")
        if not simple(pts):
            raise SystemExit(f"{name}: a polygon crosses itself")
    return shapes


def c_glyph(cname, shapes, lines):
    xs = [p[0] for _, pts in shapes for p in pts]
    ys = [p[1] for _, pts in shapes for p in pts]
    for i, (tone, pts) in enumerate(shapes):
        flat = ", ".join(f"{round(x * UNIT)}, {round(y * UNIT)}" for x, y in pts)
        lines.append(f"static const int16_t {cname}_p{i}[] = {{ {flat} }};")
    polys = ", ".join(f"{{ {tone}, {len(pts)}, {cname}_p{i} }}" for i, (tone, pts) in enumerate(shapes))
    lines.append(f"static const hud_glyph_poly {cname}_polys[] = {{ {polys} }};")
    return (f"{{ {len(shapes)}, {cname}_polys, {round(min(xs) * UNIT)}, {round(min(ys) * UNIT)}, "
            f"{round(max(xs) * UNIT)}, {round(max(ys) * UNIT)} }}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--swift", help="a local ManeuversStyleKit.swift (else downloaded)")
    ap.add_argument("--out", default=OUT)
    ap.add_argument("--json", help="also write the shapes as JSON, for a preview")
    a = ap.parse_args()
    if a.swift:
        with open(a.swift, "rb") as f:
            raw = f.read()
    else:
        raw = urllib.request.urlopen(URL, timeout=30).read()
    if hashlib.sha256(raw).hexdigest() != SHA256:
        raise SystemExit("ManeuversStyleKit.swift is not the pinned v0.40.0 file")
    funcs = functions(raw.decode("utf-8"))
    lines, entries, js, total = [], [], {}, 0
    for name in GLYPHS:
        shapes = glyph(funcs, name)
        entries.append(c_glyph("g_" + name.lower(), shapes, lines))
        js[name] = shapes
        total += len(shapes)
    rb = []
    for ang in ROUNDABOUT_ANGLES:
        shapes = glyph(funcs, "Roundabout", roundabout_angle=ang)
        rb.append(c_glyph(f"g_roundabout_{ang}", shapes, lines))
        js[f"Roundabout{ang}"] = shapes
        total += len(shapes)
    enum = ",\n    ".join(f"HUD_GLYPH_{n.upper()}" for n in GLYPHS)
    text = f"""/* GENERATED by tools/hud_glyphs_gen.py from ManeuversStyleKit.swift of mapbox-navigation-ios v0.40.0
 * (sha256 {SHA256[:16]}…). Do not edit; regenerate. {len(GLYPHS)} glyphs and {len(ROUNDABOUT_ANGLES)} roundabout exits, {total} polygons.
 *
 * The drawings are Mapbox's, under this notice:
 *
{chr(10).join(' * ' + l if l else ' *' for l in NOTICE.splitlines())}
 *
 * Units: 1/{UNIT} of Mapbox's 32-unit canvas, y down. Tone 0 = route, 1 = context (grey), 2 = hole (context tone, on top).
 */
#ifndef HUD_GLYPHS_H
#define HUD_GLYPHS_H

#include <stdint.h>

typedef struct {{ uint8_t tone, n; const int16_t *xy; }} hud_glyph_poly;
typedef struct {{ int n; const hud_glyph_poly *poly; int16_t x0, y0, x1, y1; }} hud_glyph;

enum {{
    {enum},
    HUD_GLYPH_COUNT
}};
enum {{ HUD_GLYPH_UNIT = {UNIT}, HUD_ROUNDABOUT_STEP = {ROUNDABOUT_ANGLES[1] - ROUNDABOUT_ANGLES[0]}, HUD_ROUNDABOUT_FIRST = {ROUNDABOUT_ANGLES[0]},
       HUD_ROUNDABOUT_COUNT = {len(ROUNDABOUT_ANGLES)} }};

{chr(10).join(lines)}

/* indexed by HUD_GLYPH_* */
static const hud_glyph hud_glyphs[HUD_GLYPH_COUNT] = {{
    {(','+chr(10)+'    ').join(entries)}
}};
/* exit angle HUD_ROUNDABOUT_FIRST + i * HUD_ROUNDABOUT_STEP degrees, anticlockwise traffic, 90 = first right */
static const hud_glyph hud_roundabouts[HUD_ROUNDABOUT_COUNT] = {{
    {(','+chr(10)+'    ').join(rb)}
}};

#endif
"""
    with open(a.out, "w", encoding="utf-8") as f:
        f.write(text)
    if a.json:
        with open(a.json, "w") as f:
            json.dump(js, f)
    print(f"{len(GLYPHS)} glyphs, {len(ROUNDABOUT_ANGLES)} roundabouts, {total} polygons, "
          f"max {max(len(p) for v in js.values() for _, p in v)} points -> {a.out}")


if __name__ == "__main__":
    sys.exit(main())
