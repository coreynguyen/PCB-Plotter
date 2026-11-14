#!/usr/bin/env python3
"""hlc_diff.py - field-by-field diff of two JUKI HLC programs (H4H/H5H/H7H/H8H/E4x).

usage:  python3 -I tools/hlc_diff.py A.h8h B.h8h [--raw] [--max N]
        python3 -I tools/hlc_diff.py A.h8h            (dump one file)

Every named field comes from specs/HLC_FORMAT.md (JUKI MemoryConversion IL,
Juki Tool, TTP). Placements are matched by reference designator, components by
part number, so record order does not matter. --raw additionally lists every
differing byte offset inside each record that is NOT a named field (unknown
bytes), which is how a new field is pinned against a JUKI-written file.
"""
import struct
import sys

def u8(d, o): return d[o]
def u16(d, o): return struct.unpack_from('<H', d, o)[0]
def i16(d, o): return struct.unpack_from('<h', d, o)[0]
def u32(d, o): return struct.unpack_from('<I', d, o)[0]
def i32(d, o): return struct.unpack_from('<i', d, o)[0]
def cstr(d, o, n):
    b = d[o:o + n].split(b'\0')[0]
    return b.decode('latin-1').rstrip(' ')

BOARD = [  # (offset, reader, name)
    (0, lambda d, o: cstr(d, o, 32), 'pwb_id'),
    (34, u32, 'last_edited'), (38, u32, 'last_optimized'),
    (42, u16, 'mode(+42)'),
    (44, i32, 'pwb_w'), (48, i32, 'pwb_l'), (52, i32, 'ref_pos_x'), (56, i32, 'ref_pos_y'),
    (60, i32, 'layout_offset_x'), (64, i32, 'layout_offset_y'), (68, u16, 'pwb_setflags'),
    (70, i32, 'ckt_w'), (74, i32, 'ckt_l'), (78, i32, 'ckt_layout_x'), (82, i32, 'ckt_layout_y'),
    (86, u16, 'ckt_setflags'),
    (160, i16, 'matrix_div_x'), (162, i16, 'matrix_div_y'), (164, i32, 'matrix_start_x'),
    (168, i32, 'matrix_start_y'), (172, i32, 'matrix_pitch_x'), (176, i32, 'matrix_pitch_y'),
    (180, u16, 'matrix_setflags'), (182, u8, 'nonmatrix_count'),
    (2584, i16, 'place_height'), (2596, i16, 'boc_mark_index[0]'), (2598, i16, 'boc_mark_index[1]'), (2600, i16, 'boc_mark_index[2]'),
    (10668, u8, 'pwb_type'), (10669, i32, 'clamp_offset_y'), (10673, i32, 'back_height'),
    (10807, i32, 'pwb_thickness'), (10811, u16, 'pwb_flags2(+10811)'), (10825, i32, '+10825'),
]
PLACE = [
    (0, lambda d, o: cstr(d, o, 6), 'ref6'), (6, u8, 'data_ok'), (8, i32, 'x'), (12, i32, 'y'),
    (16, i16, 'angle80'), (18, u8, 'layer'), (19, u8, 'machine_no'), (20, u8, 'opt_machine_no'),
    (23, i16, 'opt_place_no'), (27, u16, 'attr(+27)'), (29, u16, 'attr2(+29)'),
    (31, u16, 'status(+31)'), (33, i16, 'comp_index'), (35, i16, 'area_fid_index'), (38, u8, 'circuit_index'),
]
COMP = [
    (0, lambda d, o: cstr(d, o, 20), 'part'), (47, lambda d, o: cstr(d, o, 30), 'comment'),
    (78, u16, 'type'), (80, u8, 'feeder_kind'), (81, i32, 'outline_w'), (85, i32, 'outline_l'),
    (89, i32, 'outline_h'), (93, i32, 'connector_h'), (113, u16, 'outline_setflags'),
    (117, u8, '+117'), (118, u16, 'placement_count'),
]
NAMED_BOARD = set()
for o, f, n in BOARD:
    w = 32 if n == 'pwb_id' else (1 if f is u8 else 2 if f in (u16, i16) else 4)
    NAMED_BOARD.update(range(o, o + w))

def width(f, n):
    if n in ('pwb_id',): return 32
    if n == 'ref6': return 6
    if n == 'part': return 20
    if n == 'comment': return 30
    return 1 if f is u8 else 2 if f in (u16, i16) else 4

def load(path):
    d = open(path, 'rb').read()
    a0 = u32(d, 38)
    n = (a0 - 38) // 16
    blocks = [struct.unpack_from('<4I', d, 38 + 16 * i) for i in range(n)]
    return d, blocks

def rec(d, blocks, k, i):
    a, pre, rs, cnt = blocks[k]
    return a + pre + rs * i

def fields(d, base, table, size):
    out = {}
    for o, f, n in table:
        if o + width(f, n) <= size:
            out[n] = f(d, base + o)
    return out

def header(d, blocks):
    h = {'name': cstr(d, 0, 32), 'station': d[32], 'writer': d[33], 'version': d[34:38].rstrip(b'\0').decode('latin-1'),
         'n_blocks': len(blocks)}
    for i, b in enumerate(blocks):
        if any(b): h['blk%02d' % i] = b
    a, _, rs, _ = blocks[1]
    if a and rs >= 602:
        h['lineinfo+54'] = d[a + 54:a + 62]
        for k in range(27):
            e = struct.unpack_from('<5I', d, a + 62 + 20 * k)
            if any(e): h['li%02d' % k] = e
    return h

def summary(path):
    d, b = load(path)
    out = {'header': header(d, b)}
    out['board'] = fields(d, b[2][0], BOARD, b[2][2]) if b[2][0] else {}
    cfg = (out['board'].get('mode(+42)', 0) >> 3) & 3
    out['board']['circuit_config'] = ['Single PWB', 'Matrix', 'Non-Matrix', '?'][cfg]
    nc = out['board'].get('nonmatrix_count', 0)
    for k in range(min(nc, 200)):
        o = b[2][0] + 184 + 12 * k
        out['board']['circuit%03d' % (k + 1)] = (i32(d, o), i32(d, o + 4), i16(d, o + 8), u16(d, o + 10))
    pl = {}
    for i in range(b[3][3]):
        r = rec(d, b, 3, i)
        f = fields(d, r, PLACE, b[3][2])
        name = cstr(d, rec(d, b, 12, i) + 1, 11) if i < b[12][3] else f.get('ref6', '')
        pl[name or ('#%d' % i)] = (r, f)
    cm = {}
    for i in range(b[4][3]):
        r = rec(d, b, 4, i)
        f = fields(d, r, COMP, b[4][2])
        cm[f['part'] or ('#%d' % i)] = (r, f)
    out['placements'] = pl
    out['components'] = cm
    a, pre, rs, cnt = b[6]
    marks = []
    if a and cnt and rs in (3072, 552, 536):
        es = 128 if rs == 3072 else 124
        for m in range(cnt):
            e = a + rs * m
            n = u16(d, e + 18)
            marks.append((d[e], d[e + 1], cstr(d, e + 2, 16), n,
                          [(i32(d, e + 40 + es * k), i32(d, e + 44 + es * k), u16(d, e + 48 + es * k)) for k in range(min(n, 4))]))
    out['marks'] = marks
    return d, b, out

def diff_dict(tag, A, B, lines):
    for k in sorted(set(A) | set(B), key=str):
        if A.get(k) != B.get(k):
            lines.append('  %-28s %-30s %s' % (tag + k, A.get(k, '<none>'), B.get(k, '<none>')))

def raw_diff(da, ra, db, rb, size, named, lines, tag, maxn):
    offs = [o for o in range(size) if o not in named and da[ra + o] != db[rb + o]]
    if offs:
        lines.append('  %s unknown bytes differ at %s%s' % (tag, offs[:maxn], ' ...' if len(offs) > maxn else ''))

def block_hexdiff(da, ba, db, bb, lines, maxn):
    """--raw: block table entries and every block byte-by-byte (incl. unnamed
    blocks 0, 5..11, 13..27), reported per in-record offset (HLC_FORMAT.md §11)."""
    if len(da) != len(db):
        lines.append('  file size %d vs %d' % (len(da), len(db)))
    for k in range(max(len(ba), len(bb))):
        ea = ba[k] if k < len(ba) else None
        eb = bb[k] if k < len(bb) else None
        if ea != eb:
            lines.append('  blocktable[%02d] %s vs %s' % (k, ea, eb))
        if not ea or not eb or not ea[0] or not eb[0]:
            continue
        na, nb = ea[1] + ea[2] * ea[3], eb[1] + eb[2] * eb[3]
        n = min(na, nb)
        pre, rs = ea[1], max(ea[2], 1)
        offs = {}
        for i in range(n):
            if da[ea[0] + i] != db[eb[0] + i]:
                key = ('pre+%d' % i) if i < pre else '%d' % ((i - pre) % rs)
                if key not in offs:
                    offs[key] = (i, da[ea[0] + i], db[eb[0] + i], 0)
                o = offs[key]
                offs[key] = (o[0], o[1], o[2], o[3] + 1)
        if offs:
            items = sorted(offs.items(), key=lambda kv: kv[1][0])
            desc = ', '.join('+%s:%02x/%02x%s' % (k2, v[1], v[2], ('x%d' % v[3]) if v[3] > 1 else '') for k2, v in items[:maxn])
            lines.append('  block%02d bytes (offset in record: A/B, xN records): %s%s' % (k, desc, ' ...' if len(items) > maxn else ''))
        if na != nb:
            lines.append('  block%02d length %d vs %d' % (k, na, nb))

def main(argv):
    raw = '--raw' in argv
    maxn = 40
    if '--max' in argv:
        maxn = int(argv[argv.index('--max') + 1])
    files = [a for a in argv[1:] if not a.startswith('--') and not a.isdigit()]
    if len(files) == 1:
        _, _, s = summary(files[0])
        for k, v in s['header'].items(): print('%-14s %s' % (k, v))
        for k, v in s['board'].items(): print('board.%-22s %s' % (k, v))
        for k, (r, f) in list(s['components'].items()): print('comp', k, f)
        for k, (r, f) in list(s['placements'].items())[:maxn]: print('place', k, f)
        for m in s['marks']: print('marks', m)
        return 0
    da, ba, A = summary(files[0])
    db, bb, B = summary(files[1])
    lines = []
    diff_dict('', A['header'], B['header'], lines)
    diff_dict('board.', A['board'], B['board'], lines)
    if raw and ba[2][0] and bb[2][0]:
        raw_diff(da, ba[2][0], db, bb[2][0], min(ba[2][2], bb[2][2]),
                 NAMED_BOARD | set(range(184, 184 + 2400)), lines, 'board', maxn)
    for k in sorted(set(A['components']) | set(B['components'])):
        if k not in A['components'] or k not in B['components']:
            lines.append('  component %s only in %s' % (k, 'A' if k in A['components'] else 'B'))
            continue
        (ra, fa), (rb, fb) = A['components'][k], B['components'][k]
        diff_dict('comp[%s].' % k, fa, fb, lines)
        if raw:
            named = set()
            for o, f, n in COMP: named.update(range(o, o + width(f, n)))
            named.update(range(1143, 1143 + 190))
            raw_diff(da, ra, db, rb, min(ba[4][2], bb[4][2]), named, lines, 'comp[%s]' % k, maxn)
    for k in sorted(set(A['placements']) | set(B['placements'])):
        if k not in A['placements'] or k not in B['placements']:
            lines.append('  placement %s only in %s' % (k, 'A' if k in A['placements'] else 'B'))
            continue
        (ra, fa), (rb, fb) = A['placements'][k], B['placements'][k]
        diff_dict('place[%s].' % k, fa, fb, lines)
        if raw:
            named = set()
            for o, f, n in PLACE: named.update(range(o, o + width(f, n)))
            raw_diff(da, ra, db, rb, min(ba[3][2], bb[3][2]), named, lines, 'place[%s]' % k, maxn)
    if A['marks'] != B['marks']:
        lines.append('  marks A=%s' % A['marks'])
        lines.append('  marks B=%s' % B['marks'])
    if raw:
        block_hexdiff(da, ba, db, bb, lines, maxn)
    print('%d difference(s)  (A=%s  B=%s)' % (len(lines), files[0], files[1]))
    print('\n'.join(lines))
    return 1 if lines else 0

if __name__ == '__main__':
    sys.exit(main(sys.argv))
