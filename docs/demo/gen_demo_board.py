#!/usr/bin/env python3
# Generates docs/demo/demo_board.kicad_pcb: a fictional two-sided board used for the README
# screenshots and as a hands-on sample. Not a real design; no customer data.
#   python3 docs/demo/gen_demo_board.py
import math, os, random

random.seed(7)
HERE = os.path.dirname(os.path.abspath(__file__))
W, H, R = 100.0, 70.0, 4.0

def chip(pitch_x, pw, ph, body):     # 2-pad chip: pad centre offset, pad w/h, body (w, h)
    return {"pads": [(-pitch_x, 0, pw, ph), (pitch_x, 0, pw, ph)], "body": body}

def gull(n, pitch, span, pw, ph, body):   # dual row
    half = n // 2
    y0 = (half - 1) * pitch / 2
    pads = [(-span / 2, y0 - i * pitch, pw, ph) for i in range(half)]
    pads += [(span / 2, -y0 + i * pitch, pw, ph) for i in range(half)]
    return {"pads": pads, "body": body}

def quad(n, pitch, span, pw, ph, body):   # QFP, four sides
    side = n // 4
    o = (side - 1) * pitch / 2
    pads = []
    for i in range(side): pads.append((-span / 2, o - i * pitch, pw, ph))
    for i in range(side): pads.append((-o + i * pitch, -span / 2, ph, pw))
    for i in range(side): pads.append((span / 2, -o + i * pitch, pw, ph))
    for i in range(side): pads.append((o - i * pitch, span / 2, ph, pw))
    return {"pads": pads, "body": body}

def bga(rows, pitch, d, body):
    o = (rows - 1) * pitch / 2
    return {"pads": [(-o + c * pitch, -o + r * pitch, d, d) for r in range(rows) for c in range(rows)],
            "body": body, "round": True}

FP = {
    "R_0402": chip(0.48, 0.56, 0.62, (1.0, 0.5)),
    "R_0603": chip(0.8, 0.9, 0.95, (1.6, 0.8)),
    "C_0603": chip(0.8, 0.9, 0.95, (1.6, 0.8)),
    "C_0805": chip(0.95, 1.0, 1.45, (2.0, 1.25)),
    "C_1206": chip(1.5, 1.15, 1.8, (3.2, 1.6)),
    "SOT-23": {"pads": [(-1.1, 0.95, 1.3, 0.6), (-1.1, -0.95, 1.3, 0.6), (1.1, 0, 1.3, 0.6)], "body": (1.3, 2.9)},
    "SOIC-8": gull(8, 1.27, 5.4, 1.55, 0.6, (3.9, 4.9)),
    "SOIC-16": gull(16, 1.27, 5.4, 1.55, 0.6, (3.9, 9.9)),
    "TSSOP-20": gull(20, 0.65, 5.8, 1.4, 0.4, (4.4, 6.5)),
    "QFP-64": quad(64, 0.5, 11.4, 1.5, 0.28, (10.0, 10.0)),
    "BGA-64": bga(8, 0.8, 0.4, (7.0, 7.0)),
    "LED_0805": chip(0.95, 1.0, 1.2, (2.0, 1.25)),
    "Fiducial": {"pads": [(0, 0, 1.0, 1.0)], "body": None, "round": True},
}
VAL = {"R_0402": ("100R", "RES-0402-100R"), "R_0603": ("10k", "RES-0603-10K"), "C_0603": ("100n", "CAP-0603-100N"),
       "C_0805": ("1u", "CAP-0805-1U"), "C_1206": ("10u", "CAP-1206-10U"), "SOT-23": ("NPN", "TRN-SOT23-NPN"),
       "SOIC-8": ("OPAMP", "IC-SOIC8-OPAMP"), "SOIC-16": ("DRIVER", "IC-SOIC16-DRV"), "TSSOP-20": ("BUFFER", "IC-TSSOP20-BUF"),
       "QFP-64": ("MCU", "IC-QFP64-MCU"), "BGA-64": ("FPGA", "IC-BGA64-FPGA"), "LED_0805": ("GREEN", "LED-0805-GRN"),
       "Fiducial": ("FID", "")}
PFX = {"R": "R", "C": "C", "S": "Q", "L": "D", "F": "FID"}

parts, counters = [], {}
def add(fp, x, y, rot, side, prefix=None):
    p = prefix or ("U" if fp in ("SOIC-8", "SOIC-16", "TSSOP-20", "QFP-64", "BGA-64") else PFX[fp[0]])
    counters[p] = counters.get(p, 0) + 1
    parts.append((f"{p}{counters[p]}", fp, x, y, rot, side))

# top side
add("QFP-64", 30, 35, 0, "top"); add("BGA-64", 62, 40, 0, "top")
add("SOIC-16", 82, 20, 90, "top"); add("SOIC-8", 82, 52, 0, "top"); add("TSSOP-20", 12, 50, 90, "top")
for i in range(8): add("C_0603", 18 + i * 3.2, 20, 90, "top")
for i in range(8): add("R_0603", 18 + i * 3.2, 51, 90, "top")
for i in range(6): add("R_0402", 47, 28 + i * 2.2, 0, "top")
for i in range(4): add("C_0805", 55 + i * 4, 56, 0, "top")
for i in range(3): add("C_1206", 70 + i * 6, 64, 0, "top")
for i in range(4): add("SOT-23", 70 + i * 5.5, 8, 0, "top")
for i in range(5): add("LED_0805", 10 + i * 4, 8, 180, "top")
for i in range(4): add("R_0603", 72, 28 + i * 3, 0, "top")
# bottom side
for i in range(10): add("C_0603", 26 + (i % 5) * 3, 28 + (i // 5) * 14, 0, "bottom")
for i in range(6): add("R_0603", 58 + i * 2.6, 30, 90, "bottom")
add("SOIC-8", 40, 60, 0, "bottom"); add("SOT-23", 20, 62, 90, "bottom")
for i in range(3): add("C_0805", 88, 36 + i * 4, 90, "bottom")
# fiducials (global, top + bottom at the same board positions)
for (x, y) in [(4, 4), (96, 4), (4, 66)]:
    add("Fiducial", x, y, 0, "top", "FID")
for (x, y) in [(4, 4), (96, 4), (4, 66)]:
    add("Fiducial", x, y, 0, "bottom", "FID")

def n(v): return f"{v:.4f}".rstrip("0").rstrip(".")

out = ['(kicad_pcb (version 20221018) (generator pcbnew)', '  (general (thickness 1.6))',
       '  (layers (0 "F.Cu" signal) (31 "B.Cu" signal) (44 "Edge.Cuts" user) (37 "F.SilkS" user) (36 "B.SilkS" user) (49 "F.Fab" user) (48 "B.Fab" user))']
# outline with rounded corners (KiCad y down: board spans y 0..-H)
def L(a, b): out.append(f'  (gr_line (start {n(a[0])} {n(-a[1])}) (end {n(b[0])} {n(-b[1])}) (stroke (width 0.1) (type default)) (layer "Edge.Cuts"))')
def A(s, m, e): out.append(f'  (gr_arc (start {n(s[0])} {n(-s[1])}) (mid {n(m[0])} {n(-m[1])}) (end {n(e[0])} {n(-e[1])}) (stroke (width 0.1) (type default)) (layer "Edge.Cuts"))')
k = R * (1 - math.sqrt(0.5))
L((R, 0), (W - R, 0)); A((W - R, 0), (W - k, k), (W, R)); L((W, R), (W, H - R)); A((W, H - R), (W - k, H - k), (W - R, H))
L((W - R, H), (R, H)); A((R, H), (k, H - k), (0, H - R)); L((0, H - R), (0, R)); A((0, R), (k, k), (R, 0))
# mounting holes
for (x, y) in [(96, 66)]:
    out.append(f'  (gr_circle (center {n(x)} {n(-y)}) (end {n(x + 1.6)} {n(-y)}) (stroke (width 0.1) (type default)) (layer "Edge.Cuts"))')

for (ref, fp, x, y, rot, side) in parts:
    f = FP[fp]; val, mpn = VAL[fp]
    bot = side == "bottom"
    cu, pa, ma, fab = ("B.Cu", "B.Paste", "B.Mask", "B.Fab") if bot else ("F.Cu", "F.Paste", "F.Mask", "F.Fab")
    out.append(f'  (footprint "Demo:{fp}" (layer "{cu}") (at {n(x)} {n(-y)} {n(rot)})')
    out.append(f'    (property "Reference" "{ref}" (at 0 0 {n(rot)}) (layer "{"B.SilkS" if bot else "F.SilkS"}"))')
    out.append(f'    (property "Value" "{val}" (at 0 1.5) (layer "{fab}"))')
    if mpn: out.append(f'    (property "MPN" "{mpn}")')
    if f.get("body"):
        bw, bh = f["body"]
        pts = [(-bw / 2, -bh / 2), (bw / 2, -bh / 2), (bw / 2, bh / 2), (-bw / 2, bh / 2)]
        for i in range(4):
            a, b = pts[i], pts[(i + 1) % 4]
            if bot: a, b = (-a[0], a[1]), (-b[0], b[1])
            out.append(f'    (fp_line (start {n(a[0])} {n(a[1])}) (end {n(b[0])} {n(b[1])}) (layer "{fab}") (width 0.1))')
    shape = "circle" if f.get("round") else "roundrect"
    for i, (px, py, pw, ph) in enumerate(f["pads"]):
        if bot: px = -px
        extra = "" if shape == "circle" else " (roundrect_rratio 0.25)"
        layers = f'"{cu}" "{ma}"' if fp == "Fiducial" else f'"{cu}" "{pa}" "{ma}"'
        out.append(f'    (pad "{i + 1}" smd {shape} (at {n(px)} {n(-py)} {n(rot)}) (size {n(pw)} {n(ph)}) (layers {layers}){extra})')
    out.append('  )')
out.append(')')
open(os.path.join(HERE, "demo_board.kicad_pcb"), "w").write("\n".join(out) + "\n")
print(len(parts), "parts")
