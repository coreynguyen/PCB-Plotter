#!/usr/bin/env python3
# Generates the synthetic fixed-width samples DemoBOM.prn (commented-header BOM, 9 columns) and
# "Reference File.txt" (headerless centroid, ref / X / Y / rotation in mils). Fictional data only.
#   python3 tests/data/text/gen_fixed_samples.py
import os, random, re
random.seed(11)
HERE = os.path.dirname(os.path.abspath(__file__))
W = [24, 26, 16, 64, 16, 16, 16, 24]
HDR = ["Reference", "Part Number", "Type", "Description", "Process", "Cage Code", "Sofware Rev", "Vendor Part Number", "Polarity"]
L = [  # refs, pn, type, desc, process, polarity
 ("TP1-TP6", "TPT-1000-01", "TP", "SMT Test Point", "SMT", "FALSE"),
 ("R1,R2", "RES-AX-5K", "AXIAL", "Resistor, 5k Ohm", "SLIDE", "FALSE"),
 ("RN1-RN2", "RNET-SIP10-10K", "SIP10", "Resistor network, SIP package", "SLIDE", "TRUE"),
 ("J3", "HDR-6P-254", "HDR6", "Connector, 6 pin header", "SLIDE", "TRUE"),
 ("RN3", "RNET-DIP16-1K", "DIP16", "Resistor network (16 pin)", "SLIDE", "TRUE"),
 ("J1", "CONN-50P", "CONN50", "50 pin connector", "HAND", "TRUE"),
 ("J2", "CONN-40P", "CONN40", "40 pin connector", "HAND", "N"),
 ("X1", "OSC-20M-A", "CRYSTAL1", "Crystal oscillator (moisture sensitive)", "HAND", "TRUE"),
 ("X2", "OSC-20M-B", "CRYSTAL2", "Crystal oscillator (sealed)", "HAND", "TRUE"),
 ("R6-8", "TRIM-10K", "TRIMR", "Resistor (trimmable)", "SLIDE", "TRUE"),
 ("T1-T2", "XFMR-1-1", "TRANS", "Transformer", "HAND", "TRUE"),
 ("R9-R18", "RES-AX-10K", "AXIAL", "Resistor 10k Ohm", "SLIDE", "FALSE"),
 ("C1-C25,C27-C34", "CAP-0805-100N", "0805", "Capacitor (0805 SMT package)", "SMT", "FALSE"),
 ("C26", "CAP-4334-10U", "4334", "Capacitor (4334 package)", "SMT", "TRUE"),
 ("U2", "IC-DIP24W-LOGIC", "24DIPW", "Logic circuit", "AUTO", "TRUE"),
 ("U3,U11,U14", "IC-DIP24-CNT", "DIP24", "Counter", "AUTO", "TRUE"),
 ("U4", "IC-DIP16-MUX", "DIP16", "Multiplexer", "AUTO", "TRUE"),
 ("U5-U7", "IC-SOIC8-OPA", "SOIC8", "Op amp", "SMT", "TRUE"),
 ("U8", "IC-PLCC32-MEM", "PLCC32", "Memory", "SMT", "TRUE"),
 ("U9-U10", "IC-PQFP132-CPU", "PQFP132", "Processor", "SMT", "TRUE"),
 ("U12-U13", "IC-SOIC16W-DRV", "SOIC16W", "Line driver", "SMT", "TRUE"),
 ("U15", "IC-SOIC20-BUF", "SOIC20", "Buffer", "SMT", "TRUE"),
 ("U16", "IC-SOIC24-ADC", "SOIC24", "ADC", "SMT", "TRUE"),
 ("U17", "IC-SOIC32-RAM", "SOIC32", "RAM", "SMT", "TRUE"),
 ("D1-D17", "DIO-SOD123-1A", "SOD123", "Diode 1A", "SMT", "TRUE"),
]
def expand(s):
    out = []
    for part in re.split(r"\s*,\s*", s):
        m = re.match(r"([A-Z]+)(\d+)-([A-Z]*)(\d+)$", part)
        if m: out += [f"{m.group(1)}{i}" for i in range(int(m.group(2)), int(m.group(4)) + 1)]
        else: out.append(part)
    return out
def row(cells):
    return "".join(c.ljust(w) for c, w in zip(cells, W)) + cells[-1]
lines = [";Bill of Materials", ";Synthetic Demonstration Board", ";", ";" + HDR[0].ljust(W[0] - 1) + row(HDR)[W[0]:], ";"]
refs = []
for i, (r, pn, t, d, p, pol) in enumerate(L):
    lines.append(row([r, pn, t, d, p, str(1000 + 37 * i), "N/A" if i else "Rev. 01", f"V{pn}", pol]))
    refs += expand(r)
assert len(refs) == 98, len(refs)
open(os.path.join(HERE, "DemoBOM.prn"), "w", newline="\r\n").write("\n".join(lines) + "\n")
refs.sort(key=lambda r: (r != "U14", r))
out = []
first = True
for r in refs:
    if first: x, y, a = 3120, 1975, 90; first = False
    else: x, y, a = random.randrange(200, 7500, 5), random.randrange(200, 4200, 5), random.choice([0, 90, 180, 270])
    out.append(f"{r:<24}{x:<10}{y:<10}{a:<10}")
open(os.path.join(HERE, "Reference File.txt"), "w", newline="\r\n").write("\n".join(out) + "\n")
