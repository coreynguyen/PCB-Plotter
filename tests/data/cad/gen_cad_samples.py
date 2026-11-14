#!/usr/bin/env python3
"""Generates the synthetic 'same board in every CAD format' samples used by
tests/test_cad_v2.cpp.  Re-run from the repo root:  python3 tests/data/cad/gen_cad_samples.py

Board (mm, Y up, CCW degrees, bottom placement = rotate then mirror local X):
  outline 50 x 40 with an R5 rounded corner at top-right (arc centre 45,35)
  R1  R0805  (10,10)   0  top     C1  R0805 (20,10)  90 top
  R2  R0805  (40,10) 180  bottom  U1  SOIC8 (30,22)  90 bottom
  J1  HDR3   (12,30) 270  top (through-hole, drill 1.0)
  FID1 FID1MM (5,5)  0  top
"""
import math, os, io, tarfile, zipfile, gzip, shutil

OUT = os.path.dirname(os.path.abspath(__file__))
MIL = 0.0254
INCH = 25.4

# ---------------------------------------------------------------- the board
PKGS = {
    'R0805': dict(pins=[('1', -0.95, 0, 1.0, 1.3, 'rect', 0), ('2', 0.95, 0, 1.0, 1.3, 'rect', 0)]),
    'SOIC8': dict(pins=[(str(i + 1), -2.7, [1.905, 0.635, -0.635, -1.905][i], 1.5, 0.6, 'rect', 0) for i in range(4)] +
                       [(str(i + 5), 2.7, [-1.905, -0.635, 0.635, 1.905][i], 1.5, 0.6, 'rect', 0) for i in range(4)]),
    'HDR3': dict(pins=[('1', -2.54, 0, 1.7, 1.7, 'square', 1.0), ('2', 0, 0, 1.7, 1.7, 'round', 1.0),
                       ('3', 2.54, 0, 1.7, 1.7, 'round', 1.0)]),
    'FID1MM': dict(pins=[('1', 0, 0, 1.0, 1.0, 'round', 0)]),
}
COMPS = [  # ref, pkg, x, y, rot, bottom, part, value
    ('R1', 'R0805', 10, 10, 0, False, 'RC0805-10K', '10k'),
    ('C1', 'R0805', 20, 10, 90, False, 'CC0805-100N', '100nF'),
    ('R2', 'R0805', 40, 10, 180, True, 'RC0805-10K', '10k'),
    ('U1', 'SOIC8', 30, 22, 90, True, 'LM358', 'LM358'),
    ('J1', 'HDR3', 12, 30, 270, False, 'HDR-1X3', 'HDR'),
    ('FID1', 'FID1MM', 5, 5, 0, False, '', ''),
]
# outline as (kind, ...) segments in order: line a->b, arc a->b around c ccw
OUTLINE = [('L', (0, 0), (50, 0)), ('L', (50, 0), (50, 35)), ('A', (50, 35), (45, 40), (45, 35)),
           ('L', (45, 40), (0, 40)), ('L', (0, 40), (0, 0))]


def l2a(lx, ly, x, y, rot, bottom):
    if bottom:
        lx = -lx
    r = math.radians(rot)
    return (x + lx * math.cos(r) - ly * math.sin(r), y + lx * math.sin(r) + ly * math.cos(r))


def f(v, d=6):
    s = ('%.' + str(d) + 'f') % v
    s = s.rstrip('0').rstrip('.') if '.' in s else s
    return '0' if s in ('-0', '') else s


def write(name, text, nl='\n'):
    with open(os.path.join(OUT, name), 'w', newline='') as fh:
        fh.write(text.replace('\n', nl))


def pins_abs():
    for ref, pk, x, y, rot, bot, part, val in COMPS:
        for (pn, lx, ly, w, h, sh, dr) in PKGS[pk]['pins']:
            ax, ay = l2a(lx, ly, x, y, rot, bot)
            yield ref, pn, ax, ay, w, h, sh, dr, bot

# ---------------------------------------------------------------- ODB++ (INCH)
def odb():
    root = os.path.join(OUT, 'odb_synth')
    shutil.rmtree(root, ignore_errors=True)
    def put(rel, text):
        p = os.path.join(root, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'w', newline='') as fh:
            fh.write(text)
    put('matrix/matrix', '''STEP {
   COL=1
   NAME=PCB
}

LAYER {
   ROW=1
   CONTEXT=BOARD
   TYPE=COMPONENT
   NAME=COMP_+_TOP
   POLARITY=POSITIVE
}

LAYER {
   ROW=2
   CONTEXT=BOARD
   TYPE=SIGNAL
   NAME=TOP
   POLARITY=POSITIVE
}

LAYER {
   ROW=3
   CONTEXT=BOARD
   TYPE=SIGNAL
   NAME=BOTTOM
   POLARITY=POSITIVE
}

LAYER {
   ROW=4
   CONTEXT=BOARD
   TYPE=COMPONENT
   NAME=COMP_+_BOT
   POLARITY=POSITIVE
}

LAYER {
   ROW=5
   CONTEXT=BOARD
   TYPE=DRILL
   NAME=DRILL
   POLARITY=POSITIVE
}
''')
    I = lambda v: f(v / INCH, 6)
    prof = ['UNITS=INCH', '#', '#Num Features', '#', 'S P 0']
    first = True
    for seg in OUTLINE:
        if first:
            prof.append('OB %s %s I' % (I(seg[1][0]), I(seg[1][1])))
            first = False
        if seg[0] == 'L':
            prof.append('OS %s %s' % (I(seg[2][0]), I(seg[2][1])))
        else:
            prof.append('OC %s %s %s %s N' % (I(seg[2][0]), I(seg[2][1]), I(seg[3][0]), I(seg[3][1])))
    prof += ['OE', 'SE', '']
    put('steps/pcb/profile', '\n'.join(prof))
    eda = ['HDR synth eda', 'UNITS=INCH', 'LYR top bottom', '#', 'NET $NONE$', '#']
    order = ['R0805', 'SOIC8', 'HDR3', 'FID1MM']
    for i, n in enumerate(order):
        eda.append('# PKG %d' % i)
        eda.append('PKG %s 0.05 -0.1 -0.1 0.1 0.1;;ID=%d' % (n, i))
        eda.append('RC -0.1 -0.1 0.2 0.2')
        for (pn, lx, ly, w, h, sh, dr) in PKGS[n]['pins']:
            eda.append('PIN %s %s %s %s 0 U U' % (pn, 'T' if dr else 'S', I(lx), I(ly)))
            if sh == 'round':
                eda.append('CR %s %s %s' % (I(lx), I(ly), I(w / 2)))
            elif sh == 'square':
                eda.append('SQ %s %s %s' % (I(lx), I(ly), I(w / 2)))
            else:
                eda.append('RC %s %s %s %s' % (I(lx - w / 2), I(ly - h / 2), I(w), I(h)))
    put('steps/pcb/eda/data', '\n'.join(eda) + '\n')
    for side in ('top', 'bot'):
        out = ['UNITS=INCH', '#']
        for ci, (ref, pk, x, y, rot, bot, part, val) in enumerate(COMPS):
            if bot != (side == 'bot'):
                continue
            cw = (-rot) % 360
            out.append('# CMP %d' % ci)
            out.append('CMP %d %s %s %s %s %s %s;0=0' % (order.index(pk), I(x), I(y), f(cw), 'M' if bot else 'N', ref, part or 'FIDUCIAL'))
            if val:
                out.append("PRP VALUE '%s'" % val)
            for k, (pn, lx, ly, w, h, sh, dr) in enumerate(PKGS[pk]['pins']):
                ax, ay = l2a(lx, ly, x, y, rot, bot)
                out.append('TOP %d %s %s %s %s 0 0 %s' % (k, I(ax), I(ay), f(cw), 'M' if bot else 'N', pn))
            out.append('#')
        put('steps/pcb/layers/comp_+_%s/components' % side, '\n'.join(out) + '\n')
    drl = ['UNITS=INCH', '$0 r39.370079', '#']
    for ref, pn, ax, ay, w, h, sh, dr, bot in pins_abs():
        if dr:
            drl.append('P %s %s 0 P 0 0' % (I(ax), I(ay)))
    put('steps/pcb/layers/drill/features', '\n'.join(drl) + '\n')
    # archives (job folder name 'synth' inside)
    with tarfile.open(os.path.join(OUT, 'odb_synth.tgz'), 'w:gz', format=tarfile.USTAR_FORMAT) as tf:
        tf.add(root, arcname='synth')
    with zipfile.ZipFile(os.path.join(OUT, 'odb_synth.zip'), 'w', zipfile.ZIP_DEFLATED) as zf:
        for d, _, files in os.walk(root):
            for fn in sorted(files):
                p = os.path.join(d, fn)
                zf.write(p, 'synth/' + os.path.relpath(p, root).replace(os.sep, '/'))

# ---------------------------------------------------------------- IPC-2581 (mm)
def ipc():
    prims = {}
    def prim(w, h, sh):
        key = '%s_%sx%s' % (sh.upper(), f(w), f(h))
        prims[key] = (w, h, sh)
        return key
    pk_xml = []
    for n, d in PKGS.items():
        th = any(p[6] for p in d['pins'])
        pk_xml.append('      <Package name="%s" type="OTHER" pinOne="1" height="1.0">' % n)
        pk_xml.append('        <Outline><Polygon><PolyBegin x="-1" y="-1"/><PolyStepSegment x="1" y="-1"/><PolyStepSegment x="1" y="1"/>'
                      '<PolyStepSegment x="-1" y="1"/><PolyStepSegment x="-1" y="-1"/></Polygon><LineDesc lineWidth="0.1"/></Outline>')
        for (pn, lx, ly, w, h, sh, dr) in d['pins']:
            pk_xml.append('        <Pin number="%s" type="%s" electricalType="ELECTRICAL"><Location x="%s" y="%s"/>'
                          '<StandardPrimitiveRef id="%s"/></Pin>' % (pn, 'THRU' if dr else 'SURFACE', f(lx), f(ly), prim(w, h, sh)))
        pk_xml.append('      </Package>')
    comp_xml = []
    for ref, pk, x, y, rot, bot, part, val in COMPS:
        comp_xml.append('      <Component refDes="%s" packageRef="%s" layerRef="%s" part="%s" mountType="%s">'
                        '<Xform rotation="%s"%s/><Location x="%s" y="%s"/></Component>'
                        % (ref, pk, 'BOTTOM' if bot else 'TOP', part, 'THMT' if pk == 'HDR3' else 'SMT', f(rot),
                           ' mirror="true"' if bot else '', f(x), f(y)))
    bom = {}
    for ref, pk, x, y, rot, bot, part, val in COMPS:
        if part:
            bom.setdefault((part, val), []).append((ref, pk, bot))
    bom_xml = []
    for (part, val), refs in bom.items():
        bom_xml.append('    <BomItem OEMDesignNumberRef="%s" quantity="%d" category="ELECTRICAL">' % (part, len(refs)))
        for ref, pk, bot in refs:
            bom_xml.append('      <RefDes name="%s" packageRef="%s" populate="true" layerRef="%s"/>' % (ref, pk, 'BOTTOM' if bot else 'TOP'))
        bom_xml.append('      <Characteristics category="ELECTRICAL"><Textual definitionSource="SUPPLIER" '
                       'textualCharacteristicName="VALUE" textualCharacteristicValue="%s"/></Characteristics>' % val)
        bom_xml.append('    </BomItem>')
    prof = []
    for i, seg in enumerate(OUTLINE):
        if i == 0:
            prof.append('<PolyBegin x="%s" y="%s"/>' % (f(seg[1][0]), f(seg[1][1])))
        if seg[0] == 'L':
            prof.append('<PolyStepSegment x="%s" y="%s"/>' % (f(seg[2][0]), f(seg[2][1])))
        else:
            prof.append('<PolyStepCurve x="%s" y="%s" centerX="%s" centerY="%s" clockwise="false"/>'
                        % (f(seg[2][0]), f(seg[2][1]), f(seg[3][0]), f(seg[3][1])))
    holes = ['          <Hole name="H%d" diameter="%s" platingStatus="PLATED" plusTol="0" minusTol="0" x="%s" y="%s"/>'
             % (i, f(dr), f(ax), f(ay)) for i, (ref, pn, ax, ay, w, h, sh, dr, bot) in enumerate(pins_abs()) if dr]
    dict_xml = []
    for k, (w, h, sh) in prims.items():
        if sh == 'round':
            dict_xml.append('    <EntryStandard id="%s"><Circle diameter="%s"/></EntryStandard>' % (k, f(w)))
        else:
            dict_xml.append('    <EntryStandard id="%s"><RectCenter width="%s" height="%s"/></EntryStandard>' % (k, f(w), f(h)))
    x = '''<?xml version="1.0" encoding="UTF-8"?>
<!-- synthetic sample for PCB Plotter tests -->
<IPC-2581 revision="B" xmlns="http://webstds.ipc.org/2581" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
  <Content roleRef="Owner">
    <FunctionMode mode="ASSEMBLY"/>
    <StepRef name="board"/>
    <LayerRef name="TOP"/>
    <LayerRef name="BOTTOM"/>
    <BomRef name="synth_bom"/>
    <DictionaryStandard units="MILLIMETER">
%s
    </DictionaryStandard>
  </Content>
  <LogisticHeader><Role id="Owner" roleFunction="SENDER"/><Enterprise id="PCBP" code="NONE"/><Person name="test" enterpriseRef="PCBP" roleRef="Owner"/></LogisticHeader>
  <Bom name="synth_bom">
    <BomHeader assembly="synth" revision="1"><StepRef name="board"/></BomHeader>
%s
  </Bom>
  <Ecad name="synth">
    <CadHeader units="MILLIMETER"><Spec name="default"/></CadHeader>
    <CadData>
      <Layer name="TOP" layerFunction="CONDUCTOR" side="TOP" polarity="POSITIVE"/>
      <Layer name="BOTTOM" layerFunction="CONDUCTOR" side="BOTTOM" polarity="POSITIVE"/>
      <Layer name="DRILL_1_2" layerFunction="DRILL" side="ALL" polarity="POSITIVE"/>
      <Step name="board">
      <Datum x="0" y="0"/>
      <Profile><Polygon>%s</Polygon></Profile>
%s
%s
      <LayerFeature layerRef="DRILL_1_2">
        <Set>
%s
        </Set>
      </LayerFeature>
      </Step>
    </CadData>
  </Ecad>
</IPC-2581>
''' % ('\n'.join(dict_xml), '\n'.join(bom_xml), ''.join(prof), '\n'.join(pk_xml), '\n'.join(comp_xml), '\n'.join(holes))
    write('synth.ipc2581.xml', x)

# ---------------------------------------------------------------- KiCad (mm, Y down)
def kicad():
    o = ['(kicad_pcb (version 20221018) (generator pcbnew)', '  (general (thickness 1.6))',
         '  (layers (0 "F.Cu" signal) (31 "B.Cu" signal) (44 "Edge.Cuts" user))']
    K = lambda v: f(v)
    for seg in OUTLINE:
        a, b = seg[1], seg[2]
        if seg[0] == 'L':
            o.append('  (gr_line (start %s %s) (end %s %s) (stroke (width 0.1) (type default)) (layer "Edge.Cuts"))'
                     % (K(a[0]), K(-a[1]), K(b[0]), K(-b[1])))
        else:
            c = seg[3]
            r = math.hypot(a[0] - c[0], a[1] - c[1])
            a0 = math.atan2(a[1] - c[1], a[0] - c[0]); a1 = math.atan2(b[1] - c[1], b[0] - c[0])
            am = (a0 + a1) / 2
            m = (c[0] + r * math.cos(am), c[1] + r * math.sin(am))
            o.append('  (gr_arc (start %s %s) (mid %s %s) (end %s %s) (stroke (width 0.1) (type default)) (layer "Edge.Cuts"))'
                     % (K(a[0]), K(-a[1]), K(m[0]), K(-m[1]), K(b[0]), K(-b[1])))
    # a silkscreen line that must be ignored
    o.append('  (gr_line (start 0 0) (end 10 -10) (layer "F.SilkS") (width 0.12))')
    for ref, pk, x, y, rot, bot, part, val in COMPS:
        legacy = ref == 'J1'
        kw = 'module' if legacy else 'footprint'
        o.append('  (%s "Lib:%s" (layer "%s") (at %s %s%s)' % (kw, pk, 'B.Cu' if bot else 'F.Cu', K(x), K(-y),
                                                              (' ' + f(rot)) if rot else ''))
        if legacy:
            o.append('    (fp_text reference %s (at 0 -2) (layer F.SilkS))' % ref)
            o.append('    (fp_text value %s (at 0 2) (layer F.Fab))' % val)
        else:
            o.append('    (property "Reference" "%s" (at 0 -1.5 %s) (layer "%s.SilkS"))' % (ref, f(rot), 'B' if bot else 'F'))
            o.append('    (property "Value" "%s" (at 0 1.5) (layer "F.Fab"))' % val)
            if part:
                o.append('    (property "MPN" "%s")' % part)
        if pk == 'FID1MM':
            o.append('    (attr smd board_only exclude_from_pos_files)')
        o.append('    (fp_line (start -1 -1) (end 1 -1) (layer "F.SilkS") (width 0.12))')
        for (pn, lx, ly, w, h, sh, dr) in PKGS[pk]['pins']:
            # KiCad local coords (Y down); bottom footprints are stored flipped
            kx, ky = (-lx, -ly) if bot else (lx, -ly)
            shape = {'rect': 'roundrect', 'square': 'rect', 'round': 'circle'}[sh]
            typ = 'thru_hole' if dr else 'smd'
            lay = '"*.Cu" "*.Mask"' if dr else ('"B.Cu" "B.Paste" "B.Mask"' if bot else '"F.Cu" "F.Paste" "F.Mask"')
            o.append('    (pad "%s" %s %s (at %s %s%s) (size %s %s)%s (layers %s)%s)'
                     % (pn, typ, shape, K(kx), K(ky), (' ' + f(rot)) if rot else '', K(w), K(h),
                        (' (drill %s)' % K(dr)) if dr else '', lay, ' (roundrect_rratio 0.25)' if shape == 'roundrect' else ''))
        o.append('  )')
    # a mounting hole footprint without pins must not break anything
    o.append('  (footprint "MountingHole:MH3" (layer "F.Cu") (at 45 -5) (property "Reference" "H1") (property "Value" "MH")'
             ' (pad "" np_thru_hole circle (at 0 0) (size 3.2 3.2) (drill 3.2) (layers "*.Cu" "*.Mask")))')
    o.append(')')
    write('synth.kicad_pcb', '\n'.join(o) + '\n')

# ---------------------------------------------------------------- Eagle (mm)
def eagle():
    o = ['<?xml version="1.0" encoding="utf-8"?>', '<!DOCTYPE eagle SYSTEM "eagle.dtd">', '<eagle version="9.6.2">', '<drawing>',
         '<settings><setting alwaysvectorfont="no"/></settings>', '<grid distance="0.1" unitdist="inch" unit="inch"/>',
         '<layers><layer number="1" name="Top" color="4" fill="1" visible="yes" active="yes"/>'
         '<layer number="16" name="Bottom" color="1" fill="1" visible="yes" active="yes"/>'
         '<layer number="20" name="Dimension" color="24" fill="1" visible="yes" active="yes"/></layers>', '<board>', '<plain>']
    for seg in OUTLINE:
        a, b = seg[1], seg[2]
        o.append('<wire x1="%s" y1="%s" x2="%s" y2="%s" width="0" layer="20"%s/>'
                 % (f(a[0]), f(a[1]), f(b[0]), f(b[1]), ' curve="90"' if seg[0] == 'A' else ''))
    o.append('<text x="5" y="45" size="1.778" layer="21">synth &amp; test</text>')
    o.append('</plain>')
    o.append('<libraries><library name="synth"><packages>')
    for n, d in PKGS.items():
        o.append('<package name="%s"><description>&lt;b&gt;%s&lt;/b&gt;</description>' % (n, n))
        for (pn, lx, ly, w, h, sh, dr) in d['pins']:
            if dr:
                o.append('<pad name="%s" x="%s" y="%s" drill="%s" diameter="%s"%s/>' % (pn, f(lx), f(ly), f(dr), f(w),
                                                                                      ' shape="square"' if sh == 'square' else ''))
            else:
                o.append('<smd name="%s" x="%s" y="%s" dx="%s" dy="%s" layer="1"%s/>' % (pn, f(lx), f(ly), f(w), f(h),
                                                                                       ' roundness="100"' if sh == 'round' else ''))
        o.append('<wire x1="-1" y1="-1" x2="1" y2="-1" width="0.127" layer="21"/>')
        o.append('</package>')
    o.append('</packages></library></libraries>')
    o.append('<elements>')
    for ref, pk, x, y, rot, bot, part, val in COMPS:
        r = ('M' if bot else '') + 'R' + f(rot)
        o.append('<element name="%s" library="synth" package="%s" value="%s" x="%s" y="%s"%s>'
                 % (ref, pk, val, f(x), f(y), (' rot="%s"' % r) if (rot or bot) else ''))
        if part:
            o.append('<attribute name="MPN" value="%s" x="0" y="0" size="1" layer="27" display="off"/>' % part)
        o.append('</element>')
    o.append('</elements>')
    o += ['<signals/>', '</board>', '</drawing>', '</eagle>']
    write('synth.brd', '\n'.join(o) + '\n')

# ---------------------------------------------------------------- PADS (mils)
def pads():
    M = lambda v: f(v / MIL, 4)
    o = ['!PADS-POWERPCB-V9.0-MILS! DESIGN DATABASE ASCII FILE 1.0', '',
         '*PCB*        GENERAL PARAMETERS OF THE PCB DESIGN', '', 'UNITS        0           UNITS',
         'USERGRID     1 1', '', '*REMARK* NAME UNITS ORIX ORIY ATTRS LABELS PIECES TXT TERMINALS STACKS MAXLAYERS', '',
         '*PARTDECAL*  ITEMS', '']
    for n, d in PKGS.items():
        pins = d['pins']
        o.append('%s M 0 0 0 1 1 0 %d %d 0' % (n, len(pins), len(pins)))
        o.append('VALUE 0 0 0 1 50 5 0 0 0 "Regular <Romansim Stroke Font>"')
        o.append('Part Type')
        o.append('CLOSED 5 8 26 -1')
        for c in ((-40, -40), (40, -40), (40, 40), (-40, 40), (-40, -40)):
            o.append('%d %d' % c)
        for (pn, lx, ly, w, h, sh, dr) in pins:
            o.append('T%s %s %s %s %s' % (M(lx), M(ly), M(lx), M(ly), pn))
        for (pn, lx, ly, w, h, sh, dr) in pins:
            k = pins.index((pn, lx, ly, w, h, sh, dr)) + 1
            o.append('PAD %d 3%s' % (k, (' P %s' % M(dr)) if dr else ''))
            if sh == 'rect':
                # RF: width orientation length offset (length along orientation)
                if w <= h:
                    o.append('-2 %s RF 0 90 %s 0' % (M(w), M(h)))
                else:
                    o.append('-2 %s RF 0 0 %s 0' % (M(h), M(w)))
            elif sh == 'square':
                o.append('-2 %s S 0' % M(w))
            else:
                o.append('-2 %s R' % M(w))
            o.append('-1 %s R' % (M(w) if dr else '0'))
            o.append('0 %s R' % (M(w) if dr else '0'))
        o.append('')
    o += ['*PARTTYPE*   ITEMS', '']
    seen = set()
    for ref, pk, x, y, rot, bot, part, val in COMPS:
        t = part or 'FIDUCIAL'
        if t in seen:
            continue
        seen.add(t)
        o.append('%s %s I UND 0 0 0 0 0' % (t, pk))
    o += ['', '*PART*       ITEMS', '']
    for ref, pk, x, y, rot, bot, part, val in COMPS:
        t = part or 'FIDUCIAL'
        name = t if ref in ('R1', 'J1') else '%s@%s' % (t, pk)    # some via *PARTTYPE*, some explicit
        labels = 1 if ref == 'R1' else 0
        o.append('%s %s %s %s %s U %s 0 -1 0 -1 %d' % (ref, name, M(x), M(y), f(rot), 'M' if bot else 'N', labels))
        if labels:
            o.append('VALUE 0 0 0 26 50 5 0 0 0 "Regular <Romansim Stroke Font>"')
            o.append('Part Type')
    o += ['', '*LINES*      LINES ITEMS', '']
    pts = []
    for seg in OUTLINE:
        a = seg[1]
        if seg[0] == 'A':
            c = seg[3]; r = 5
            pts.append('%s %s %d %d %s %s %s %s' % (M(a[0] - 10), M(a[1] - 5), 0, 900, M(c[0] - r - 10), M(c[1] - r - 5),
                                                   M(c[0] + r - 10), M(c[1] + r - 5)))
        else:
            pts.append('%s %s' % (M(a[0] - 10), M(a[1] - 5)))
    pts.append('%s %s' % (M(-10), M(-5)))
    o.append('BOARD_OUTLINE BOARD %s %s 1 0' % (M(10), M(5)))       # item origin (10,5): coords are relative
    o.append('CLOSED %d 10 0 -1' % len(pts))
    o += pts
    o.append('SILK_LINE LINES 0 0 1 0')
    o.append('OPEN 2 8 26 -1')
    o += ['0 0', '1000 1000']
    o += ['', '*END*     OF ASCII OUTPUT FILE']
    write('synth.asc', '\n'.join(o) + '\n', nl='\r\n')

# ---------------------------------------------------------------- Allegro extract (mils)
def allegro():
    M = lambda v: f(v / MIL, 4)
    stack = lambda w, h, sh, dr: (('PAD%sSQ%sD' % (M(w).replace('.', '_'), M(dr).replace('.', '_'))) if sh == 'square' else
                                  ('PAD%sCIR%sD' % (M(w).replace('.', '_'), M(dr).replace('.', '_')))) if dr else \
        (('C%s' % M(w).replace('.', '_')) if sh == 'round' else ('SMD%sX%s' % (M(w).replace('.', '_'), M(h).replace('.', '_'))))
    o = ['J!synth.brd!Mon Oct 26 10:00:00 2026!-100.00!-100.00!2100.00!1700.00!0.0001!MILS!4!',
         'A!REFDES!SYM_X!SYM_Y!SYM_ROTATE!SYM_MIRROR!SYM_NAME!COMP_DEVICE_TYPE!COMP_VALUE!COMP_PART_NUMBER!']
    for ref, pk, x, y, rot, bot, part, val in COMPS:
        o.append('S!%s!%s!%s!%s!%s!%s!%s_%s!%s!%s!' % (ref, M(x), M(y), f(rot), 'YES' if bot else 'NO', pk, pk, part or 'FID',
                                                     val, part))
    o.append('A!REFDES!PIN_NUMBER!PIN_X!PIN_Y!PAD_STACK_NAME!NET_NAME!')
    for ref, pn, ax, ay, w, h, sh, dr, bot in pins_abs():
        o.append('S!%s!%s!%s!%s!%s!N%s_%s!' % (ref, pn, M(ax), M(ay), stack(w, h, sh, dr), ref, pn))
    o.append('A!CLASS!SUBCLASS!GRAPHIC_DATA_NAME!GRAPHIC_DATA_NUMBER!RECORD_TAG!GRAPHIC_DATA_1!GRAPHIC_DATA_2!GRAPHIC_DATA_3!'
             'GRAPHIC_DATA_4!GRAPHIC_DATA_5!GRAPHIC_DATA_6!GRAPHIC_DATA_7!GRAPHIC_DATA_8!GRAPHIC_DATA_9!GRAPHIC_DATA_10!')
    for i, seg in enumerate(OUTLINE):
        a, b = seg[1], seg[2]
        if seg[0] == 'L':
            o.append('S!BOARD GEOMETRY!OUTLINE!LINE!%d!%d 1 0!%s!%s!%s!%s!0!!!!!!' % (i + 1, i + 1, M(a[0]), M(a[1]), M(b[0]), M(b[1])))
        else:
            c = seg[3]
            o.append('S!BOARD GEOMETRY!OUTLINE!ARC!%d!%d 1 0!%s!%s!%s!%s!%s!%s!%s!0!COUNTERCLOCKWISE!!' %
                     (i + 1, i + 1, M(a[0]), M(a[1]), M(b[0]), M(b[1]), M(c[0]), M(c[1]), M(5)))
    o.append('S!BOARD GEOMETRY!SILKSCREEN_TOP!LINE!99!99 1 0!0!0!100!100!0!!!!!!')
    write('synth_allegro.txt', '\n'.join(o) + '\n')

# ---------------------------------------------------------------- Mentor neutral (inches)
def mentor():
    I = lambda v: f(v / INCH, 6)
    st = lambda w, h, sh, dr: ('C%sD%s' % (f(w / INCH * 1000, 3), f(dr / INCH * 1000, 3))) if dr else \
        ('R%sX%s' % (f(w / INCH * 1000, 3), f(h / INCH * 1000, 3)))
    o = ['# Mentor neutral file (synthetic)', 'B_UNITS Inches', 'B_ATTR BOARD_NAME synth', 'B_ATTR BOARD_THICKNESS 0.062']
    for ref, pk, x, y, rot, bot, part, val in COMPS:
        o.append('C %s %s %s %s %s %s %s' % (ref, part or 'FIDUCIAL', pk, I(x), I(y), '2' if bot else '1', f(rot)))
        for (pn, lx, ly, w, h, sh, dr) in PKGS[pk]['pins']:
            ax, ay = l2a(lx, ly, x, y, rot, bot)
            o.append('P %s %s %s %s %s "%s" NET_%s_%s' % (ref, pn, I(ax), I(ay), 'BOTTOM' if bot and not dr else 'TOP',
                                                       st(w, h, sh, dr), ref, pn))
    write('synth.neu', '\n'.join(o) + '\n')

# ---------------------------------------------------------------- FABmaster (1/1000 inch)
def fab():
    M = lambda v: f(v / MIL, 4)
    o = [':FABMASTER FATF REV 11.1;', ':UNITS = 1/1000 INCH;', ':NOAUTOROTATE;', ':PADS']
    pads = {}
    for n, d in PKGS.items():
        for (pn, lx, ly, w, h, sh, dr) in d['pins']:
            key = (w, h, sh, dr)
            if key not in pads:
                pads[key] = len(pads) + 1
                o.append('%d,P%d,%s,%s,%s,%s;' % (pads[key], pads[key], {'rect': 'RECT', 'square': 'SQUARE', 'round': 'ROUND'}[sh],
                                                  M(w), M(h), M(dr)))
    o.append(':PARTS')
    for i, (ref, pk, x, y, rot, bot, part, val) in enumerate(COMPS):
        o.append('%d,%s,%s,%s,%s,%s,%s,%s,%s;' % (i + 1, ref, part, pk, M(x), M(y), f(rot), 'B' if bot else 'T', val))
    o.append(':PINS')
    for i, (ref, pk, x, y, rot, bot, part, val) in enumerate(COMPS):
        for (pn, lx, ly, w, h, sh, dr) in PKGS[pk]['pins']:
            ax, ay = l2a(lx, ly, x, y, rot, bot)
            o.append('%d,%s,%s,%s,%d;' % (i + 1, pn, M(ax), M(ay), pads[(w, h, sh, dr)]))
    o.append(':BOARD_OUTLINE')
    for seg in OUTLINE:
        a, b = seg[1], seg[2]
        if seg[0] == 'L':
            o.append('LINE,%s,%s,%s,%s;' % (M(a[0]), M(a[1]), M(b[0]), M(b[1])))
        else:
            c = seg[3]
            o.append('ARC,%s,%s,%s,%s,\n  %s,%s,CCW;' % (M(a[0]), M(a[1]), M(b[0]), M(b[1]), M(c[0]), M(c[1])))
    o.append(':EOF')
    write('synth.fab', '\n'.join(o) + '\n')

# ---------------------------------------------------------------- DXF
def dxf():
    def hdr(u):
        return ['0', 'SECTION', '2', 'HEADER', '9', '$ACADVER', '1', 'AC1015', '9', '$INSUNITS', '70', str(u), '0', 'ENDSEC',
                '0', 'SECTION', '2', 'TABLES', '0', 'ENDSEC', '0', 'SECTION', '2', 'ENTITIES']
    tail = ['0', 'ENDSEC', '0', 'EOF']
    e = hdr(4)
    e += ['0', 'LINE', '8', 'OUTLINE', '10', '0', '20', '0', '30', '0', '11', '50', '21', '0', '31', '0']
    e += ['0', 'LINE', '8', 'OUTLINE', '10', '50', '20', '35', '11', '50', '21', '0']          # reversed direction
    e += ['0', 'ARC', '8', 'OUTLINE', '10', '45', '20', '35', '40', '5', '50', '0', '51', '90']
    e += ['0', 'LWPOLYLINE', '8', 'OUTLINE', '90', '3', '70', '0', '10', '45', '20', '40', '10', '0', '20', '40', '10', '0', '20', '0']
    e += ['0', 'CIRCLE', '8', 'OUTLINE', '10', '5', '20', '35', '40', '1.5']                    # mounting hole
    e += ['0', 'LWPOLYLINE', '8', 'DIMENSIONS', '90', '4', '70', '1', '10', '-5', '20', '-5', '10', '55', '20', '-5',
          '10', '55', '20', '45', '10', '-5', '20', '45']
    e += ['0', 'TEXT', '8', 'DIMENSIONS', '10', '0', '20', '-8', '40', '2', '1', 'synth']
    write('synth_outline.dxf', '\n'.join(e + tail) + '\n', nl='\r\n')
    # inch variant using POLYLINE/VERTEX with a bulge for the corner arc (tan(90/4))
    I = lambda v: f(v / INCH, 8)
    bul = f(math.tan(math.radians(90) / 4), 10)
    e = hdr(1)
    e += ['0', 'POLYLINE', '8', 'BOARD', '66', '1', '70', '1', '10', '0', '20', '0', '30', '0']
    for (x, y, b) in ((0, 0, 0), (50, 0, 0), (50, 35, bul), (45, 40, 0), (0, 40, 0)):
        e += ['0', 'VERTEX', '8', 'BOARD', '10', I(x), '20', I(y), '30', '0'] + (['42', str(b)] if b else [])
    e += ['0', 'SEQEND', '8', 'BOARD']
    write('synth_outline_inch.dxf', '\n'.join(e + tail) + '\n')


# ---------------------------------------------------------------- ODB++ robustness cases (tests/data/cad/odb_cases)
def lzw_compress(data, maxbits=16):
    """Unix compress(1) .Z writer (block mode) - mirrors src/io/lzw.cpp."""
    out = bytearray(b'\x1f\x9d' + bytes([0x80 | maxbits]))
    st = dict(acc=0, n=0, bits=0, gstart=0, width=9)
    def put(code):
        st['acc'] |= code << st['n']; st['n'] += st['width']; st['bits'] += st['width']
        while st['n'] >= 8:
            out.append(st['acc'] & 0xff); st['acc'] >>= 8; st['n'] -= 8
    def pad():
        g = st['width'] * 8; used = st['bits'] - st['gstart']
        if used % g:
            k = g - used % g
            st['n'] += k; st['bits'] += k
            while st['n'] >= 8:
                out.append(st['acc'] & 0xff); st['acc'] >>= 8; st['n'] -= 8
    if not data:
        return bytes(out)
    maxmax = 1 << maxbits
    d = {}; free = 257; maxcode = 511
    w = data[0]
    for c in data[1:]:
        k = (w, c)
        if k in d:
            w = d[k]; continue
        put(w)
        if free < maxmax:
            d[k] = free; free += 1
        else:
            put(256); pad(); d = {}; st['width'] = 9; maxcode = 511; free = 257; st['gstart'] = st['bits']; w = c; continue
        if free - 1 > maxcode and st['width'] < maxbits:
            pad(); st['width'] += 1
            maxcode = maxmax - 1 if st['width'] == maxbits else (1 << st['width']) - 1
            st['gstart'] = st['bits']
        w = c
    put(w)
    if st['n'] > 0:
        out.append(st['acc'] & 0xff)
    return bytes(out)


def odb_cases():
    base = os.path.join(OUT, 'odb_cases')
    shutil.rmtree(base, ignore_errors=True)
    def job(name, files):
        root = os.path.join(base, name)
        for rel, data in files.items():
            p = os.path.join(root, rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, 'wb') as fh:
                fh.write(data if isinstance(data, bytes) else data.encode())
        return root
    def tar_bytes(root, arc, gz=True):
        bio = io.BytesIO()
        with tarfile.open(fileobj=bio, mode='w:gz' if gz else 'w', format=tarfile.USTAR_FORMAT) as tf:
            tf.add(root, arcname=arc)
        return bio.getvalue()
    def matrix(steps, layers):
        m = ''
        for i, s in enumerate(steps):
            m += 'STEP {\n   COL=%d\n   NAME=%s\n}\n\n' % (i + 1, s)
        for i, (n, t) in enumerate(layers):
            m += 'LAYER {\n   ROW=%d\n   CONTEXT=BOARD\n   TYPE=%s\n   NAME=%s\n   POLARITY=POSITIVE\n}\n\n' % (i + 1, t, n)
        return m
    def rect_profile(w, h, units='MM'):
        return 'UNITS=%s\n#\nS P 0\nOB 0 0 I\nOS %s 0\nOS %s %s\nOS 0 %s\nOS 0 0\nOE\nSE\n' % (units, f(w), f(w), f(h), f(h))
    def eda_mm():
        e = ['HDR case', 'UNITS=MM', '#']
        for n in ('R0805', 'SOIC8', 'HDR3', 'FID1MM'):
            e.append('PKG %s 1.27 -1 -1 1 1;;' % n)
            for (pn, lx, ly, w, h, sh, dr) in PKGS[n]['pins']:
                e.append('PIN %s %s %s %s 0 U U' % (pn, 'T' if dr else 'S', f(lx), f(ly)))
                e.append('RC %s %s %s %s' % (f(lx - w / 2), f(ly - h / 2), f(w), f(h)) if sh == 'rect' else
                         'CR %s %s %s' % (f(lx), f(ly), f(w / 2)))
        return '\n'.join(e) + '\n'
    order = ['R0805', 'SOIC8', 'HDR3', 'FID1MM']
    def comps_mm(bottom, ccw=False, comments=False, props=True):
        out = ['UNITS=MM', '#']
        for ci, (ref, pk, x, y, rot, bot, part, val) in enumerate(COMPS):
            if bot != bottom:
                continue
            a = rot % 360 if ccw else (-rot) % 360
            if comments:
                out.append('# CMP %d' % ci)
            out.append('CMP %d %s %s %s %s %s %s' % (order.index(pk), f(x), f(y), f(a), 'M' if bot else 'N', ref, part or 'FIDUCIAL'))
            if props and val:
                out.append("PRP VALUE '%s'" % val)
                out.append("PRP MPN 'MPN-%s'" % (part or ref))
                out.append("PRP DESCRIPTION '%s %s'" % (pk, val))
                out.append("PRP ASSEMBLY_NOTE 'x'")
            for k, (pn, lx, ly, w, h, sh, dr) in enumerate(PKGS[pk]['pins']):
                ax, ay = l2a(lx, ly, x, y, rot, bot)
                out.append('TOP %d %s %s %s %s 0 0 %s' % (k, f(ax), f(ay), f(a), 'M' if bot else 'N', pn))
        return '\n'.join(out) + '\n'
    LAY = [('comp_+_top', 'COMPONENT'), ('top', 'SIGNAL'), ('silk_top', 'SILK_SCREEN'), ('bottom', 'SIGNAL'),
           ('comp_+_bot', 'COMPONENT'), ('drill', 'DRILL')]

    # 1) panel listed first, mm, compressed members, step-repeat incl. a 90 deg (CW) instance, coupon
    files = {
        'matrix/matrix': matrix(['panel', 'pcb', 'coupon'], LAY),
        'steps/pcb/profile': rect_profile(50, 40),
        'steps/pcb/stephdr': 'UNITS=MM\nX_DATUM=0\nY_DATUM=0\n',
        'steps/pcb/eda/data.Z': lzw_compress(eda_mm().encode()),
        'steps/pcb/layers/comp_+_top/components.Z': lzw_compress(comps_mm(False).encode()),
        'steps/pcb/layers/comp_+_bot/components': comps_mm(True),
        'steps/panel/profile': rect_profile(250, 100),
        'steps/panel/stephdr': 'UNITS=MM\nX_DATUM=0\nY_DATUM=0\n'
                               'STEP-REPEAT {\nNAME=pcb\nX=10\nY=10\nDX=60\nDY=0\nNX=2\nNY=1\nANGLE=0\nMIRROR=NO\n}\n'
                               'STEP-REPEAT {\nNAME=pcb\nX=180\nY=60\nDX=0\nDY=0\nNX=1\nNY=1\nANGLE=90\nMIRROR=NO\n}\n',
        'steps/panel/layers/comp_+_top/components': 'UNITS=MM\nCMP 0 3 3 0 N PFID1 FIDUCIAL\nCMP 0 247 97 0 N PFID2 FIDUCIAL\n',
        'steps/panel/eda/data': 'UNITS=MM\nPKG FIDPANEL 1 -1 -1 1 1\nPIN 1 S 0 0 0 U U\nCR 0 0 0.5\n',
        'steps/coupon/profile': rect_profile(20, 10),
    }
    r = job('panel_mm', files)
    with open(os.path.join(base, 'panel_mm.tgz'), 'wb') as fh:
        fh.write(tar_bytes(r, 'panel_job'))

    # 2) geometry only (no component layers), nested zip > release/job.tgz, copper features .Z
    feat = ('UNITS=INCH\n$0 r10\n$1 rect60x40\n$2 s50\n$3 oval40x80\n$4 weird_user_sym\n#\n'
            'P 0.5 0.5 1 P 0 0\nP 0.6 0.5 1 P 0 1\nP 0.7 0.5 2 P 0 0\nP 0.8 0.5 3 P 0 8 30\nP 0.9 0.5 4 P 0 0\n'
            'L 0.1 0.1 1.0 0.1 0 P 0\nA 1.0 0.1 1.1 0.2 1.0 0.2 0 P 0 Y\n'
            'S P 0\nOB 0.2 0.8 I\nOS 0.4 0.8\nOS 0.4 1.0\nOS 0.2 1.0\nOE\nOB 0.25 0.85 H\nOS 0.3 0.85\nOS 0.3 0.9\nOE\nSE\n')
    silk = 'UNITS=INCH\n$0 r6\n#\nL 0 0 0.5 0 0 P 0\n'
    drl = 'UNITS=INCH\n$0 r39.37\n$1 r20 I\n#\nP 0.5 0.5 0 P 0 0\nP 0.6 0.5 1 P 0 0\n'
    files = {
        'matrix/matrix': matrix(['pcb'], [('top', 'SIGNAL'), ('silk_top', 'SILK_SCREEN'), ('bottom', 'SIGNAL'), ('drill', 'DRILL')]),
        'steps/pcb/profile': rect_profile(1.5, 1.2, 'INCH'),
        'steps/pcb/layers/top/features.Z': lzw_compress(feat.encode()),
        'steps/pcb/layers/silk_top/features': silk,
        'steps/pcb/layers/drill/features': drl,
    }
    r = job('geom_only', files)
    inner = tar_bytes(r, 'geom')
    with zipfile.ZipFile(os.path.join(base, 'geom_nested.zip'), 'w', zipfile.ZIP_DEFLATED) as zf:
        zf.writestr('release/job.tgz', inner)
        zf.writestr('release/readme.txt', 'ODB++ data inside')

    # 3) non-standard exporter: CCW angles, "# CMP" comments, no PRP; whole job as .tar.Z
    files = {
        'matrix/matrix': matrix(['pcb'], LAY),
        'steps/pcb/profile': rect_profile(50, 40),
        'steps/pcb/eda/data': eda_mm(),
        'steps/pcb/layers/comp_+_top/components': comps_mm(False, ccw=True, comments=True, props=False),
        'steps/pcb/layers/comp_+_bot/components': comps_mm(True, ccw=True, comments=True, props=False),
    }
    r = job('ccw', files)
    with open(os.path.join(base, 'ccw.tar.Z'), 'wb') as fh:
        fh.write(lzw_compress(tar_bytes(r, 'ccwjob', gz=False)))

    # 4) no eda/data: packages from toeprints; legacy 'U MM' units; gz member
    c = comps_mm(False).replace('UNITS=MM', 'U MM')
    files = {
        'matrix/matrix': matrix(['pcb'], LAY),
        'steps/pcb/profile': rect_profile(50, 40),
        'steps/pcb/layers/comp_+_top/components.gz': gzip.compress(c.encode(), mtime=0),
        'steps/pcb/layers/comp_+_bot/components': comps_mm(True),
    }
    job('noeda', files)

    # 5) panel only has a profile; components only in 'pcb' -> wrong-step diagnostics
    files = {
        'matrix/matrix': matrix(['panel', 'pcb'], LAY),
        'steps/panel/profile': rect_profile(120, 60),
        'steps/pcb/profile': rect_profile(50, 40),
        'steps/pcb/eda/data': eda_mm(),
        'steps/pcb/layers/comp_+_top/components': comps_mm(False),
        'steps/pcb/layers/comp_+_bot/components': comps_mm(True),
    }
    job('wrongstep', files)


if __name__ == '__main__':
    odb(); odb_cases(); ipc(); kicad(); eagle(); pads(); allegro(); mentor(); fab(); dxf()
    print('samples written to', OUT)
