#!/usr/bin/env python3
# Generates the synthetic §14 end-to-end fixture (REVIEW_V16): a small 2-up panelised KiCad project with
# repeated reference designators, its centroid (.pos CSV), BOM CSV and Gerbers. No customer data.
# Re-run from the repo root:  python3 tests/data/e2e/gen_e2e.py
# Conventions (match pcbp::Project::comp_xform): placement world = T(pos) * R(rot) * [S(-1,1) bottom] * local,
# Y up, mm, rotation CCW. Gerber sets are exported through a known misregistration G (plot -> Gerber):
#   e2e-*        G = translate(100, 50)                         (wrong origin, the common case)
#   e2e_rot90-*  G = translate(150, 20) * rot(90)
#   e2e_mir-*    G = translate(-30, 10) * rot(30) * mirrorX
import math, os

HERE = os.path.dirname(os.path.abspath(__file__))

FOOT = {   # local pads (x, y, w, h) Y up, pin order
    "SOT-23": [(-1.1, 0.95, 1.325, 0.6), (-1.1, -0.95, 1.325, 0.6), (1.1, 0.0, 1.325, 0.6)],
    "SOIC-8": [(-2.7, 1.905 - 1.27 * i, 1.55, 0.6) for i in range(4)] + [(2.7, -1.905 + 1.27 * i, 1.55, 0.6) for i in range(4)],
    "R_0603": [(-0.8, 0.0, 0.9, 0.95), (0.8, 0.0, 0.9, 0.95)],
    "C_0805": [(-0.95, 0.0, 1.0, 1.45), (0.95, 0.0, 1.0, 1.45)],
    "Fiducial": [(0.0, 0.0, 1.0, 1.0)],
}
# one circuit (circuit coordinates); the panel repeats it twice with the SAME references
CIRCUIT = [  # ref, footprint, value, mpn, x, y, rot, side
    ("U1", "SOT-23", "BC817", "BC817-40", 12.0, 15.0, 90.0, "top"),
    ("U2", "SOIC-8", "LM358", "LM358DR", 25.0, 12.0, 0.0, "top"),
    ("R1", "R_0603", "10k", "RC0603-10K", 30.0, 22.0, 0.0, "top"),
    ("R2", "R_0603", "10k", "RC0603-10K", 8.0, 6.0, 270.0, "top"),
    ("C1", "C_0805", "1u", "CL21B105", 20.0, 24.0, 180.0, "top"),
    ("R3", "R_0603", "4k7", "RC0603-4K7", 34.0, 8.0, 90.0, "bottom"),
]
CIRC_ORIGINS = [(5.0, 5.0), (55.0, 5.0)]    # panel 100 x 40, circuits 40 x 30, pitch 50
PANEL = (0.0, 0.0, 100.0, 40.0)
FIDS = [("FID1", 2.5, 2.5), ("FID2", 97.5, 37.5)]

def placements():
    out = []
    for (ox, oy) in CIRC_ORIGINS:
        for (ref, fp, val, mpn, x, y, r, side) in CIRCUIT:
            out.append((ref, fp, val, mpn, x + ox, y + oy, r, side))
    for (ref, x, y) in FIDS:
        out.append((ref, "Fiducial", "FID", "", x, y, 0.0, "top"))
    return out

def xf(tx, ty, rot, mir):
    c, s = math.cos(math.radians(rot)), math.sin(math.radians(rot))
    def f(p):
        x, y = p
        if mir: x = -x
        return (c * x - s * y + tx, s * x + c * y + ty)
    return f

def pad_corners(px, py, rot, side, lx, ly, w, h):
    place = xf(px, py, rot, side == "bottom")
    return [place((lx + dx, ly + dy)) for dx, dy in ((-w / 2, -h / 2), (w / 2, -h / 2), (w / 2, h / 2), (-w / 2, h / 2))]

def fmt(v): return str(int(round(v * 1e6)))

def gerber(name, items, G, lines=()):
    """items: list of corner quads (world) or ('circle', centre, d). G: plot -> gerber map."""
    out = ["G04 synthetic PCB Plotter e2e fixture: %s*" % name, "%FSLAX46Y46*%", "%MOMM*%", "%LPD*%"]
    aps, body = {}, []
    def ap(key, spec):
        if key not in aps:
            aps[key] = 10 + len(aps)
            out.append("%%ADD%d%s*%%" % (aps[key], spec))
        return aps[key]
    for it in items:
        if it[0] == "circle":
            _, c, d = it
            g = G(c)
            body.append("D%d*" % ap(("C", round(d, 4)), "C,%.4f" % d))
            body.append("X%sY%sD03*" % (fmt(g[0]), fmt(g[1])))
            continue
        q = [G(p) for p in it]
        xs, ys = [p[0] for p in q], [p[1] for p in q]
        bw, bh = max(xs) - min(xs), max(ys) - min(ys)
        e0 = math.hypot(q[1][0] - q[0][0], q[1][1] - q[0][1]); e1 = math.hypot(q[2][0] - q[1][0], q[2][1] - q[1][1])
        if abs(bw * bh - e0 * e1) < 1e-6:   # axis aligned: rectangle flash
            body.append("D%d*" % ap(("R", round(bw, 4), round(bh, 4)), "R,%.4fX%.4f" % (bw, bh)))
            body.append("X%sY%sD03*" % (fmt((min(xs) + max(xs)) / 2), fmt((min(ys) + max(ys)) / 2)))
        else:                               # rotated pad: region
            body.append("G36*")
            body.append("X%sY%sD02*" % (fmt(q[0][0]), fmt(q[0][1])))
            for p in q[1:] + q[:1]: body.append("G01X%sY%sD01*" % (fmt(p[0]), fmt(p[1])))
            body.append("G37*")
    if lines:
        body.append("D%d*" % ap(("C", 0.1), "C,0.1000"))
        for poly in lines:
            g = [G(p) for p in poly]
            body.append("X%sY%sD02*" % (fmt(g[0][0]), fmt(g[0][1])))
            for p in g[1:]: body.append("G01X%sY%sD01*" % (fmt(p[0]), fmt(p[1])))
    out += body + ["M02*", ""]
    return "\n".join(out)

def rect_poly(x0, y0, x1, y1): return [(x0, y0), (x1, y0), (x1, y1), (x0, y1), (x0, y0)]

def write(name, text):
    with open(os.path.join(HERE, name), "w", newline="\n") as f: f.write(text)

def main():
    P = placements()
    # ---- centroid (KiCad "Generate Placement Files", CSV, mm) ----
    rows = ["Ref,Val,Package,PosX,PosY,Rot,Side"]
    for (ref, fp, val, mpn, x, y, r, side) in P:
        rows.append('"%s","%s","%s",%.4f,%.4f,%.4f,%s' % (ref, val, fp, x, y, r, side))
    write("e2e-pos.csv", "\n".join(rows) + "\n")
    # ---- BOM (one line per value/footprint, schematic references: each listed ONCE for both circuits) ----
    bom = ['"Reference","Value","Footprint","MPN","Qty"']
    groups = {}
    for (ref, fp, val, mpn, x, y, r, side) in CIRCUIT:
        groups.setdefault((val, fp, mpn), []).append(ref)
    for (val, fp, mpn), refs in groups.items():
        bom.append('"%s","%s","%s","%s",%d' % (",".join(refs), val, fp, mpn, len(refs)))
    write("e2e-bom.csv", "\n".join(bom) + "\n")
    # ---- Gerbers ----
    top = [pad_corners(x, y, r, s, *pd) for (ref, fp, v, m, x, y, r, s) in P if s == "top" and fp != "Fiducial" for pd in FOOT[fp]]
    bot = [pad_corners(x, y, r, s, *pd) for (ref, fp, v, m, x, y, r, s) in P if s == "bottom" for pd in FOOT[fp]]
    fid = [("circle", (x, y), 1.0) for (ref, fp, v, m, x, y, r, s) in P if fp == "Fiducial"]
    edges = [rect_poly(*PANEL)] + [rect_poly(ox, oy, ox + 40, oy + 30) for (ox, oy) in CIRC_ORIGINS]
    for tag, G in (("e2e", xf(100, 50, 0, False)), ("e2e_rot90", xf(150, 20, 90, False)), ("e2e_mir", xf(-30, 10, 30, True))):
        write(tag + "-F_Paste.gbr", gerber("F.Paste", top, G))
        write(tag + "-F_Cu.gbr", gerber("F.Cu", top + fid, G))
        write(tag + "-B_Paste.gbr", gerber("B.Paste", bot, G))
        write(tag + "-Edge_Cuts.gbr", gerber("Edge.Cuts", [], G, edges))
    # ---- KiCad board (panel, repeated references; KiCad Y axis down) ----
    k = ['(kicad_pcb (version 20221018) (generator pcbnew)', '  (general (thickness 1.6))',
         '  (layers (0 "F.Cu" signal) (31 "B.Cu" signal) (44 "Edge.Cuts" user))']
    k.append('  (gr_rect (start %.4f %.4f) (end %.4f %.4f) (stroke (width 0.1) (type solid)) (layer "Edge.Cuts"))' % (PANEL[0], -PANEL[3], PANEL[2], -PANEL[1]))
    for (ref, fp, val, mpn, x, y, r, side) in P:
        lay = "B.Cu" if side == "bottom" else "F.Cu"
        k.append('  (footprint "Synthetic:%s" (layer "%s") (at %.4f %.4f %g)' % (fp, lay, x, -y, r))
        k.append('    (property "Reference" "%s") (property "Value" "%s")%s' % (ref, val, (' (property "MPN" "%s")' % mpn) if mpn else ""))
        for i, (lx, ly, w, h) in enumerate(FOOT[fp]):
            sx = -lx if side == "bottom" else lx    # KiCad stores bottom footprints mirrored
            shape = "circle" if fp == "Fiducial" else "rect"
            k.append('    (pad "%d" smd %s (at %.4f %.4f %g) (size %.4f %.4f) (layers "%s"))' % (i + 1, shape, sx, -ly, r, w, h, "B.Cu" if side == "bottom" else "F.Cu"))
        k.append('  )')
    k.append(')')
    write("e2e.kicad_pcb", "\n".join(k) + "\n")

if __name__ == "__main__":
    main()
