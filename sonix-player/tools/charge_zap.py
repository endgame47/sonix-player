#!/usr/bin/env python3
"""
Flattens the bolt of assets/icons/zap-device-off.svg into the polygon the
charging screen draws (src/system/device/chargescreen.c, ZAP_POINTS).

The screen is drawn before LVGL or any SVG renderer runs, so the outline is
kept as points in the icon's own 24-unit box and filled there, with the icon's
2-unit round stroke added as a distance. Re-run only when the icon changes:

    python3 tools/charge_zap.py

and paste the table it prints over ZAP_POINTS. Only the commands that path
uses are understood: M, L, H, V, A and Z, absolute and relative.
"""

import math
import os
import re

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SVG = os.path.join(REPO_ROOT, "assets", "icons", "zap-device-off.svg")

# Segments per arc: the corners are at most 1.5 units across.
ARC_STEPS = 6


def arc_points(x1, y1, rx, ry, phi, large, sweep, x2, y2):
    """The arc as points after its start, by the SVG implementation notes (F.6.5)."""
    if rx == 0 or ry == 0:
        return [(x2, y2)]
    c, s = math.cos(math.radians(phi)), math.sin(math.radians(phi))
    dx, dy = (x1 - x2) / 2, (y1 - y2) / 2
    xp, yp = c * dx + s * dy, -s * dx + c * dy
    rx, ry = abs(rx), abs(ry)
    lam = xp * xp / (rx * rx) + yp * yp / (ry * ry)
    if lam > 1:
        rx, ry = rx * math.sqrt(lam), ry * math.sqrt(lam)
    num = rx * rx * ry * ry - rx * rx * yp * yp - ry * ry * xp * xp
    den = rx * rx * yp * yp + ry * ry * xp * xp
    k = math.sqrt(max(0.0, num / den)) * (-1 if large == sweep else 1)
    cxp, cyp = k * rx * yp / ry, -k * ry * xp / rx
    cx = c * cxp - s * cyp + (x1 + x2) / 2
    cy = s * cxp + c * cyp + (y1 + y2) / 2

    def angle(ux, uy, vx, vy):
        a = math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)
        return a

    t1 = angle(1, 0, (xp - cxp) / rx, (yp - cyp) / ry)
    dt = angle((xp - cxp) / rx, (yp - cyp) / ry, (-xp - cxp) / rx, (-yp - cyp) / ry)
    if not sweep and dt > 0:
        dt -= 2 * math.pi
    elif sweep and dt < 0:
        dt += 2 * math.pi
    out = []
    for i in range(1, ARC_STEPS + 1):
        t = t1 + dt * i / ARC_STEPS
        out.append((cx + rx * math.cos(t) * c - ry * math.sin(t) * s, cy + rx * math.cos(t) * s + ry * math.sin(t) * c))
    return out


def flatten(d):
    tokens = re.findall(r"[MmLlHhVvAaZz]|-?(?:\d+\.?\d*|\.\d+)(?:e-?\d+)?", d)
    pts, x, y, i, cmd = [], 0.0, 0.0, 0, None

    def num():
        nonlocal i
        i += 1
        return float(tokens[i - 1])

    while i < len(tokens):
        if re.match(r"[A-Za-z]", tokens[i]):
            cmd = tokens[i]
            i += 1
            if cmd in "Zz":
                continue
        rel = cmd.islower()
        c = cmd.upper()
        if c in "ML":
            nx, ny = num(), num()
            x, y = (x + nx, y + ny) if rel else (nx, ny)
            pts.append((x, y))
            if c == "M":
                cmd = "l" if rel else "L"
        elif c == "H":
            nx = num()
            x = x + nx if rel else nx
            pts.append((x, y))
        elif c == "V":
            ny = num()
            y = y + ny if rel else ny
            pts.append((x, y))
        elif c == "A":
            # A flag may run into the next number ("00-2.474"): one digit each.
            rx, ry, phi = num(), num(), num()
            flags = tokens[i]
            if len(flags) > 1 and flags[0] in "01" and flags[1] in "01":
                large, sweep = int(flags[0]), int(flags[1])
                rest = flags[2:]
                if rest:
                    tokens[i] = rest
                else:
                    i += 1
            else:
                large = int(num())
                sweep = int(num())
            nx, ny = num(), num()
            ex, ey = (x + nx, y + ny) if rel else (nx, ny)
            pts.extend(arc_points(x, y, rx, ry, phi, large, sweep, ex, ey))
            x, y = ex, ey
    # The last point closes onto the first.
    if pts and math.hypot(pts[0][0] - pts[-1][0], pts[0][1] - pts[-1][1]) < 1e-6:
        pts.pop()
    return pts


def main():
    svg = open(SVG, encoding="utf-8").read()
    d = re.search(r'\bd="([^"]+)"', svg).group(1)
    pts = flatten(d)
    print("static const float ZAP_POINTS[][2] = {")
    for k in range(0, len(pts), 4):
        row = ", ".join("{%.3ff, %.3ff}" % p for p in pts[k : k + 4])
        print("\t" + row + ",")
    print("};")


if __name__ == "__main__":
    main()
