#!/usr/bin/env python3
"""cpf_report.py - human summary of a CircuitCAM .cpf/.mdb (mdbtools CLI required).

usage: python3 -I tools/cpf_report.py <file.cpf>

Prints table row counts (exported with -b hex so blobs cannot split rows),
Assembly header, Locations statistics (units mil -> mm, sides, fiducials,
features, bbox), package usage, factory model / machine setups (MachType names),
ModelFilters and the ComponentAssignments blob summary.
"""
import collections, csv, io, os, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cc_blob_dump as cbd  # noqa: E402

csv.field_size_limit(1 << 30)
MIL = 0.0254
MACH = {0: 'NONE', 1: 'Custom Machine', 12: 'MYDATA Basic', 22: 'Fuji CP', 25: 'MYDATA Advanced',
        33: 'Siemens SIPLACE', 57: 'Juki HLC', 92: 'Mirtec AOI', 96: 'Juki IS'}


def run(args):
    return subprocess.run(args, check=True, capture_output=True).stdout.decode('latin-1')


def rows(db, t):
    try:
        return list(csv.DictReader(io.StringIO(run(['mdb-export', '-b', 'hex', db, t]))))
    except subprocess.CalledProcessError:
        return []


def num(v, d=0.0):
    try:
        return float(v)
    except (TypeError, ValueError):
        return d


def blob(h):
    return bytes.fromhex(h) if h else b''


def main(argv):
    if len(argv) != 2:
        print(__doc__); return 2
    db = argv[1]
    tables = [t for t in run(['mdb-tables', '-1', db]).split('\n') if t]
    data = {t: rows(db, t) for t in tables}
    print('== %s' % db)
    print('tables with rows: ' + ', '.join('%s=%d' % (t, len(data[t])) for t in tables if data[t]))

    for a in data.get('Assembly', []):
        print('Assembly: name=%r number=%r model=%r org=(%s,%s) created=%s' % (
            a.get('Name'), a.get('Number'), a.get('Model'), a.get('OrgX'), a.get('OrgY'), a.get('Created')))
        h = a.get('ComponentAssignments')
        if h:
            pl, _ = cbd.unwrap(blob(h))
            ca = cbd.decode_comp_assignments(pl)
            for p in ca['points']:
                print('  ComponentAssignments point key=%s name=%r auto=%s assignments=%d' % (
                    p['f16'], p['name'], p['f36'], len(p['assignments'])))
        for col in ('Outline', 'Gerber', 'ScannedData', 'BalanceInfo', 'GlobalPrintSettings'):
            if a.get(col):
                print('  %s: %d bytes' % (col, len(blob(a[col]))))

    locs = data.get('Locations', [])
    if locs:
        side = collections.Counter(l['LayerID'] for l in locs)
        fid = collections.Counter(l['FiducialType'] for l in locs)
        feat = collections.Counter(l['FeatureType'] for l in locs)
        print('Locations: %d  LayerID %s  FiducialType %s  FeatureType %s' % (len(locs), dict(side), dict(fid), dict(feat)))
        print('  with Number: %d, with Package: %d, X!=XC/Y!=YC: %d, MachRot!=0: %d' % (
            sum(1 for l in locs if l['Number']), sum(1 for l in locs if l['Package']),
            sum(1 for l in locs if l['X'] != l['XC'] or l['Y'] != l['YC']), sum(1 for l in locs if num(l['MachRot']))))
        print('  Rot histogram: %s' % dict(collections.Counter(l['Rot'] for l in locs).most_common(8)))
        for s in sorted(side):
            xs = [num(l['X']) * MIL for l in locs if l['LayerID'] == s]
            ys = [num(l['Y']) * MIL for l in locs if l['LayerID'] == s]
            print('  side %s bbox mm: (%.2f, %.2f) - (%.2f, %.2f)  = %.2f x %.2f' % (
                s, min(xs), min(ys), max(xs), max(ys), max(xs) - min(xs), max(ys) - min(ys)))
        FT = {'1': 'Master/global', '2': 'Image/circuit', '3': 'Local/placement', '4': 'Zero-Point'}
        for l in locs:
            if l['FiducialType'] not in ('', '0'):
                print('  FID %-6s %-16s no=%s owner=%s x=%9.3f y=%9.3f side=%s image=%s' % (
                    l['Ref'], FT.get(l['FiducialType'], l['FiducialType']), l['FiducialNumber'], l['LocalFidID'],
                    num(l['X']) * MIL, num(l['Y']) * MIL, l['LayerID'], l['ImageID']))
        for l in locs[:10]:
            print('  %-8s x=%9.3f y=%9.3f rot=%5s side=%s pkg=%s num=%s' % (
                l['Ref'], num(l['X']) * MIL, num(l['Y']) * MIL, l['Rot'], l['LayerID'], l['Package'], l['Number']))

    pk = data.get('Packages', [])
    used = collections.Counter(l['Package'] for l in locs)
    names = {p['Package'] for p in pk}
    print('Packages: %d rows, %d with Rendering, %d used by Locations, %d unresolved' % (
        len(pk), sum(1 for p in pk if p.get('Rendering')), len(used), sum(1 for u in used if u and u not in names)))
    print('Numbers: %d rows' % len(data.get('Numbers', [])))
    def frame(h, what):
        if not h:
            return
        try:
            o = cbd.decode(blob(h))
            o = o[0] if isinstance(o, tuple) else o
            xs, ys = [], []
            def visit(n):
                for p in (n.get('pts') if isinstance(n, dict) else getattr(n, 'pts', [])) or []:
                    xs.append(p[0]); ys.append(p[1])
                for c in (n.get('children') if isinstance(n, dict) else getattr(n, 'children', [])) or []:
                    visit(c)
            visit(o)
            if xs:
                print('%s: (%.0f,%.0f)-(%.0f,%.0f) mil = (%.3f,%.3f)-(%.3f,%.3f) mm, %.3f x %.3f mm' % (
                    what, min(xs), min(ys), max(xs), max(ys), min(xs) * MIL, min(ys) * MIL, max(xs) * MIL, max(ys) * MIL,
                    (max(xs) - min(xs)) * MIL, (max(ys) - min(ys)) * MIL))
            else:
                print('%s: %d lines (no points found)' % (what, len(cbd.walk(o))))
        except Exception as e:  # noqa
            print('%s: decode failed (%s)' % (what, e))
    for a in data.get('Assembly', []):
        frame(a.get('Outline'), 'Panel frame (Assembly.Outline)')
    for i in data.get('Images', []):
        frame(i.get('ImageOutline'), 'Circuit frame (Images %s %r root=%s)' % (i['ImageID'], i['Name'], i['IsRoot']))
    for i in data.get('ImageLocs', []):
        print('ImageLoc %s: (%s,%s) rot=%s root=%s xout=%s idx=(%s,%s) image=%s' % (
            i['ImageLocID'], i['X'], i['Y'], i['Rot'], i['IsRoot'], i['XOut'], i['XIndex'], i['YIndex'], i['ImageID']))

    pps = {p['ProcessPt']: p for p in data.get('ProcessPts', [])}
    ids = set()
    for m in data.get('ModelAssignments', []):
        ids.add(m['ModelAssignID'])
        p = pps.get(m['ProcessPt'], {})
        print('ModelAssign %s seq=%s point=%r model=%r type=%r MachineType=%s AssignMode=%s' % (
            m['ModelAssignID'], m['Sequence'], m['ProcessPt'], m['Model'], p.get('ProcessType'),
            p.get('MachineType'), p.get('AssignMode')))
    for s in data.get('MachineSetups', []):
        mt = int(num(s['MachType']))
        print('MachineSetup %s -> assign %s: MachType %d (%s) file=%r dir=%r orient=%s flipped=%s off=(%s,%s) units=%s side=%s cad=%s' % (
            s['MachineSetupID'], s['ModelAssignID'], mt, MACH.get(mt, '?'), s['TargetFile'], s['TargetDir'],
            s['Orientation'], s['Flipped'], s['XOffset'], s['YOffset'], s['OffUnits'], s['ProgSide'], s['UseCadCentroids']))
        if s.get('MachSetup'):
            pl, _ = cbd.unwrap(blob(s['MachSetup']))
            d = cbd.decode_mach_setup(pl)
            print('  MachSetup blob ver=%s name=%r blobs=%d tail=%d bytes' % (d['ver'], d['name'], len(d['blobs']), len(d['rest']) // 2))
    mf = data.get('ModelFilters', [])
    if mf:
        live = sum(1 for f in mf if f['ModelAssignID'] in ids)
        print('ModelFilters: %d rows, %d for existing model points' % (len(mf), live))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
