#!/usr/bin/env python3
"""Generates SYNTHETIC email / document fixtures for tests/test_mail.cpp.

Writes tests/data/mail/: sample1.eml, sample_1250.eml, sample1.msg (HTML-in-RTF body,
recipients, attachments incl. an embedded message and a >4 KiB stream), sample2.msg
(string8 props, plain RTF body), sample1.docx.
The .msg files are hand-built MS-CFB v3 compound files (no Outlook / olefile writer);
when olefile is installed it is used as an independent oracle to validate them.
All content is invented: no customer data.
"""
import io, os, struct, sys, zipfile, zlib, binascii

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests", "data", "mail")

# ---------------------------------------------------------------------------- tiny PNG
def png(w, h, rgb):
    raw = b"".join(b"\x00" + bytes(rgb) * w for _ in range(h))
    def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")

PNG = png(4, 3, (200, 30, 30))

# ---------------------------------------------------------------------------- LZFu compressor
PRE = (b"{\\rtf1\\ansi\\mac\\deff0\\deftab720{\\fonttbl;}{\\f0\\fnil \\froman \\fswiss \\fmodern \\fscript \\fdecor MS Sans SerifSymbolArialTimes New RomanCourier"
       b"{\\colortbl\\red0\\green0\\blue0\r\n\\par \\pard\\plain\\f0\\fs20\\b\\i\\u\\tab\\tx")
assert len(PRE) == 207

def lzfu(data):
    d = bytearray(4096); d[:207] = PRE; wp = 207
    out = bytearray(); i = 0; done = False
    while not done:
        ctl = 0; grp = bytearray()
        for bit in range(8):
            if i >= len(data):
                ctl |= 1 << bit; grp += struct.pack(">H", (wp & 4095) << 4); done = True; break
            best, boff = 0, 0
            for off in range(4096):
                if off == (wp & 4095) or 1 <= ((wp - off) % 4096) <= 17: continue   # no overlapping copies
                n = 0
                while n < 17 and i + n < len(data) and d[(off + n) & 4095] == data[i + n]: n += 1
                if n > best: best, boff = n, off
                if best == 17: break
            if best >= 2:
                ctl |= 1 << bit; grp += struct.pack(">H", boff << 4 | (best - 2))
                for k in range(best): d[wp & 4095] = data[i + k]; wp += 1
                i += best
            else:
                grp.append(data[i]); d[wp & 4095] = data[i]; wp += 1; i += 1
        out.append(ctl); out += grp
    crc = binascii.crc32(bytes(out)) & 0xFFFFFFFF  # [MS-OXRTFCP] uses CRC-32 without pre/post inversion; reader does not verify
    return struct.pack("<IIII", len(out) + 12, len(data), 0x75465A4C, crc) + bytes(out)

# ---------------------------------------------------------------------------- CFB writer
class Node:
    def __init__(self, name, data=None):
        self.name, self.data, self.kids = name, data, []
    def stream(self, name, data): self.kids.append(Node(name, data)); return self
    def storage(self, name): n = Node(name); self.kids.append(n); return n

def cfb(root):
    SS = 512
    entries = []  # (node, type)
    def flat(n, t):
        idx = len(entries); entries.append([n, t, -1, -1, -1, 0, 0])
        kid_idx = [flat(k, 1 if k.data is None else 2) for k in n.kids]
        # degenerate tree: child -> first, right -> next
        if kid_idx:
            entries[idx][4] = kid_idx[0]
            for a, b in zip(kid_idx, kid_idx[1:]): entries[a][3] = b
        return idx
    flat(root, 5)
    mini = bytearray(); minifat = []
    big = []  # (entry idx, data)
    for e in entries:
        n = e[0]
        if e[1] != 2: continue
        if len(n.data) < 4096:
            start = len(mini) // 64
            nsec = max(1, (len(n.data) + 63) // 64) if n.data else 0
            if nsec:
                for k in range(nsec): minifat.append(start + k + 1 if k < nsec - 1 else 0xFFFFFFFE)
                mini += n.data + b"\0" * (nsec * 64 - len(n.data))
                e[5] = start
            else: e[5] = 0xFFFFFFFE
            e[6] = len(n.data)
        else:
            big.append(e)
    sectors = []  # list of bytes
    fat = []
    def alloc(data):
        n = (len(data) + SS - 1) // SS; start = len(sectors)
        for k in range(n):
            sectors.append(data[k * SS:(k + 1) * SS].ljust(SS, b"\0"))
            fat.append(start + k + 1 if k < n - 1 else 0xFFFFFFFE)
        return start if n else 0xFFFFFFFE
    for e in big: e[5] = alloc(e[0].data); e[6] = len(e[0].data)
    ministart = alloc(bytes(mini)) if mini else 0xFFFFFFFE
    entries[0][5] = ministart; entries[0][6] = len(mini)
    mfbytes = b"".join(struct.pack("<I", v) for v in minifat)
    mfbytes += b"\xff\xff\xff\xff" * ((-len(minifat)) % 128)
    mfstart = alloc(mfbytes) if minifat else 0xFFFFFFFE
    nmf = len(mfbytes) // SS
    def dirent(e):
        n, t, l, r, c, st, sz = e
        nm = (n.name + "\0").encode("utf-16-le")
        b = nm.ljust(64, b"\0") + struct.pack("<HBB", len(nm), t, 1) + struct.pack("<iii", l, r, c)
        b += b"\0" * 16 + b"\0" * 4 + b"\0" * 16 + struct.pack("<IIi", st if t != 1 else 0, sz if t != 1 else 0, 0)
        assert len(b) == 128, len(b)
        return b
    db = b"".join(dirent(e) for e in entries)
    db += (b"\0" * 64 + struct.pack("<HBB", 0, 0, 0) + struct.pack("<iii", -1, -1, -1) + b"\0" * 36 + struct.pack("<IIi", 0, 0, 0)) * ((-len(entries)) % 4)
    dirstart = alloc(db)
    # FAT sectors (account for themselves)
    nfat = 1
    while (len(fat) + nfat) > nfat * 128: nfat += 1
    fatstart = len(sectors)
    for k in range(nfat): fat.append(0xFFFFFFFD)
    fb = b"".join(struct.pack("<I", v) for v in fat) + b"\xff\xff\xff\xff" * (nfat * 128 - len(fat))
    for k in range(nfat): sectors.append(fb[k * SS:(k + 1) * SS])
    assert nfat <= 109
    hdr = bytes.fromhex("D0CF11E0A1B11AE1") + b"\0" * 16 + struct.pack("<HHHHH", 0x3E, 3, 0xFFFE, 9, 6) + b"\0" * 6
    hdr += struct.pack("<IIIIIIIII", 0, nfat, dirstart, 0, 4096, mfstart, nmf, 0xFFFFFFFE, 0)
    difat = [fatstart + k for k in range(nfat)] + [0xFFFFFFFF] * (109 - nfat)
    hdr += b"".join(struct.pack("<I", v) for v in difat)
    assert len(hdr) == 512
    return hdr + b"".join(sectors)

def u16(s): return s.encode("utf-16-le")
def sid(i, t): return "__substg1.0_%04X%04X" % (i, t)
def props(entries, header):
    b = header
    for tag_type, tag_id, val in entries:
        b += struct.pack("<HHI", tag_type, tag_id, 6) + val.ljust(8, b"\0")
    return b
def filetime(y, mo, d, h, mi):
    import datetime
    dt = datetime.datetime(y, mo, d, h, mi, tzinfo=datetime.timezone.utc)
    return struct.pack("<Q", int((dt.timestamp() + 11644473600) * 10000000))

def msg1():
    root = Node("Root Entry")
    root.stream("__properties_version1.0", props([(0x0003, 0x3FDE, struct.pack("<i", 65001)), (0x0040, 0x0039, filetime(2026, 3, 14, 9, 26))],
                                               b"\0" * 8 + struct.pack("<IIII", 2, 2, 3, 0) + b"\0" * 8))
    root.stream(sid(0x0037, 0x1F), u16("Quote — BOM rev B ✓"))
    root.stream(sid(0x0C1A, 0x1F), u16("Alice Example"))
    root.stream(sid(0x5D01, 0x1F), u16("alice@example.com"))
    root.stream(sid(0x0E04, 0x1F), u16("Bob Builder"))
    root.stream(sid(0x1000, 0x1F), u16("Hello World plain body\r\nLine 2"))
    html = ('<html><head><style>p{color:red}</style><script>alert(1)</script></head><body><p>Hello <b>World</b> &amp; caf\\u233?</p>'
            '<img src="cid:img1@example">{\\*\\htmltag84 &nbsp;}</body></html>')
    rtf = ("{\\rtf1\\ansi\\ansicpg1252\\fromhtml1 \\deff0{\\fonttbl{\\f0\\fswiss Arial;}}"
           "{\\*\\htmltag19 <html>}{\\*\\htmltag34 <head>}{\\*\\htmltag177 <style>p{color:red}</style>}{\\*\\htmltag41 </head>}"
           "{\\*\\htmltag50 <body>}{\\*\\htmltag64 <p>}\\htmlrtf {\\htmlrtf0 Hello }{\\*\\htmltag84 <b>}\\htmlrtf\\b \\htmlrtf0 World"
           "{\\*\\htmltag92 </b>}\\htmlrtf\\b0 \\htmlrtf0  &amp; caf\\'e9{\\*\\htmltag72 </p>}\\htmlrtf\\par \\htmlrtf0"
           "{\\*\\htmltag84 <img src=\"cid:img1@example\">}{\\*\\htmltag58 </body>}{\\*\\htmltag27 </html>}}")
    root.stream(sid(0x1009, 0x102), lzfu(rtf.encode("latin-1")))
    r0 = root.storage("__recip_version1.0_#00000000")
    r0.stream("__properties_version1.0", props([(0x0003, 0x0C15, struct.pack("<i", 1))], b"\0" * 8))
    r0.stream(sid(0x3001, 0x1F), u16("Bob Builder")); r0.stream(sid(0x39FE, 0x1F), u16("bob@example.com"))
    r1 = root.storage("__recip_version1.0_#00000001")
    r1.stream("__properties_version1.0", props([(0x0003, 0x0C15, struct.pack("<i", 2))], b"\0" * 8))
    r1.stream(sid(0x3001, 0x1F), u16("Carol QA")); r1.stream(sid(0x39FE, 0x1F), u16("carol@example.com"))
    a0 = root.storage("__attach_version1.0_#00000000")
    a0.stream("__properties_version1.0", props([(0x0003, 0x3705, struct.pack("<i", 1))], b"\0" * 8))
    a0.stream(sid(0x3707, 0x1F), u16("bom.csv")); a0.stream(sid(0x370E, 0x1F), u16("text/csv"))
    a0.stream(sid(0x3701, 0x102), b"Ref,Qty,Part\r\nR1,2,10k 0603\r\nC5,1,100n 0402\r\n")
    a1 = root.storage("__attach_version1.0_#00000001")
    a1.stream("__properties_version1.0", props([(0x0003, 0x3705, struct.pack("<i", 1)), (0x0003, 0x3714, struct.pack("<i", 4))], b"\0" * 8))
    a1.stream(sid(0x3707, 0x1F), u16("logo.png")); a1.stream(sid(0x370E, 0x1F), u16("image/png"))
    a1.stream(sid(0x3712, 0x1F), u16("img1@example")); a1.stream(sid(0x3701, 0x102), PNG)
    a2 = root.storage("__attach_version1.0_#00000002")
    a2.stream("__properties_version1.0", props([(0x0003, 0x3705, struct.pack("<i", 1))], b"\0" * 8))
    a2.stream(sid(0x3707, 0x1F), u16("big.bin"))
    a2.stream(sid(0x3701, 0x102), bytes((i * 7 + 3) & 255 for i in range(10000)))
    a3 = root.storage("__attach_version1.0_#00000003")
    a3.stream("__properties_version1.0", props([(0x0003, 0x3705, struct.pack("<i", 5))], b"\0" * 8))
    a3.stream(sid(0x3001, 0x1F), u16("Forwarded note"))
    em = a3.storage(sid(0x3701, 0x0D))
    em.stream("__properties_version1.0", props([], b"\0" * 24))
    em.stream(sid(0x0037, 0x1F), u16("Inner message"))
    em.stream(sid(0x0C1A, 0x1F), u16("Dave Inner"))
    em.stream(sid(0x1000, 0x1F), u16("Inner body text"))
    ia = em.storage("__attach_version1.0_#00000000")
    ia.stream("__properties_version1.0", props([(0x0003, 0x3705, struct.pack("<i", 1))], b"\0" * 8))
    ia.stream(sid(0x3707, 0x1F), u16("inner.txt")); ia.stream(sid(0x3701, 0x102), b"inner attachment")
    return cfb(root)

def msg2():
    root = Node("Root Entry")
    root.stream("__properties_version1.0", props([(0x0003, 0x3FFD, struct.pack("<i", 1252))], b"\0" * 32))
    root.stream(sid(0x0037, 0x1E), "R\xe9union planning".encode("cp1252"))
    root.stream(sid(0x0C1A, 0x1E), b"Eve Sender")
    rtf = ("{\\rtf1\\ansi\\ansicpg1252\\deff0{\\fonttbl{\\f0\\fswiss Arial;}{\\f1\\fmodern Courier New;}}{\\colortbl;\\red255\\green0\\blue0;}"
           "\\pard\\b Bold\\b0  normal \\i italic\\i0  {\\cf1 red} caf\\'e9 \\u8364?\\par"
           "{\\field{\\*\\fldinst HYPERLINK \"https://example.com/x\"}{\\fldrslt link text}}\\par"
           "\\trowd\\cellx1000\\cellx2000\\intbl A1\\cell B1\\cell\\row\\trowd\\cellx1000\\cellx2000\\intbl A2\\cell B2\\cell\\row\\pard after\\par}")
    root.stream(sid(0x1009, 0x102), lzfu(rtf.encode("latin-1")))
    return cfb(root)

EML1 = r"""Return-Path: <alice@example.com>
From: =?ISO-8859-1?Q?Andr=E9_Example?= <andre@example.com>
To: Bob <bob@example.com>, "Carol, QA" <carol@example.com>
Cc: =?UTF-8?B?w4l0aWVubmU=?= <et@example.com>
Subject: =?UTF-8?Q?Gerbers_+_BOM_=E2=80=94_rev?= =?UTF-8?Q?_C?=
Date: Sat, 14 Mar 2026 09:26:53 -0400
Message-ID: <abc123@example.com>
MIME-Version: 1.0
Content-Type: multipart/mixed; boundary="MIX"

This is a multi-part message in MIME format.
--MIX
Content-Type: multipart/alternative; boundary="ALT"

--ALT
Content-Type: text/plain; charset="iso-8859-1"
Content-Transfer-Encoding: quoted-printable

Hi Bob,=0D
please find the files attached. Caf=E9 soft=
 break.

--ALT
Content-Type: multipart/related; boundary="REL"

--REL
Content-Type: text/html; charset=utf-8
Content-Transfer-Encoding: base64

@@HTML@@
--REL
Content-Type: image/png; name="logo.png"
Content-Transfer-Encoding: base64
Content-ID: <logo@cid>
Content-Disposition: inline; filename="logo.png"

@@PNG@@
--REL--

--ALT--

--MIX
Content-Type: text/csv; charset=utf-8
Content-Disposition: attachment;
 filename*0*=utf-8''St%C3%BCckliste;
 filename*1=".csv"
Content-Transfer-Encoding: base64

@@CSV@@
--MIX
Content-Type: application/zip; name="=?UTF-8?Q?gerbers_r=C3=A9v.zip?="
Content-Disposition: attachment
Content-Transfer-Encoding: base64

@@ZIP@@
--MIX
Content-Type: message/rfc822

From: Dave <dave@example.com>
Subject: Original request
Content-Type: multipart/mixed; boundary="IN"

--IN
Content-Type: text/plain

Inner body
--IN
Content-Type: application/octet-stream
Content-Disposition: attachment; filename="../../evil/notes.txt"

inner file
--IN--

--MIX--
"""

HTML1 = ('<html><head><title>T</title><script>document.write("x")</script><style>.a{}</style></head>'
         '<body onload="alert(1)"><h1>Heading</h1><p>Hello <b>bold</b> <i>it</i> <a href="https://example.com/q">link</a> &amp; &euro; &#233;</p>'
         '<ul><li>one</li><li>two</li></ul><ol><li>first</li><li>second</li></ol>'
         '<table><tr><th>Ref</th><th>Qty</th></tr><tr><td>R1</td><td>2</td></tr></table>'
         '<p><img src="cid:logo@cid" alt="logo"> <img src="https://tracker.example/p.gif" width="1" height="1"> <img src="http://example.com/remote.png" alt="r"></p>'
         '<p style="color:#ff0000;font-weight:bold">red bold</p><iframe src="http://x"></iframe></body></html>')

EML2 = "From: x@example.com\r\nSubject: =?windows-1250?Q?=8Akoda_=9Ea?=\r\nContent-Type: text/plain; charset=windows-1250\r\nContent-Transfer-Encoding: 8bit\r\n\r\n".encode() + "Příliš žluťoučký kůň\r\n".encode("cp1250")

def b64(b):
    import base64
    s = base64.b64encode(b).decode()
    return "\n".join(s[i:i + 76] for i in range(0, len(s), 76))

def docx():
    W = 'xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships" xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" xmlns:wp="http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing" xmlns:pic="http://schemas.openxmlformats.org/drawingml/2006/picture"'
    doc = f'''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:document {W}><w:body>
<w:p><w:pPr><w:pStyle w:val="Heading1"/></w:pPr><w:r><w:t>Assembly notes</w:t></w:r></w:p>
<w:p><w:r><w:t xml:space="preserve">Plain </w:t></w:r><w:r><w:rPr><w:b/></w:rPr><w:t>bold</w:t></w:r><w:r><w:rPr><w:i/><w:color w:val="FF0000"/><w:sz w:val="28"/></w:rPr><w:t xml:space="preserve"> red italic 14</w:t></w:r></w:p>
<w:p><w:pPr><w:numPr><w:ilvl w:val="0"/><w:numId w:val="1"/></w:numPr></w:pPr><w:r><w:t>Step one</w:t></w:r></w:p>
<w:p><w:pPr><w:numPr><w:ilvl w:val="0"/><w:numId w:val="1"/></w:numPr></w:pPr><w:r><w:t>Step two</w:t></w:r></w:p>
<w:p><w:hyperlink r:id="rIdL"><w:r><w:t>Datasheet</w:t></w:r></w:hyperlink></w:p>
<w:tbl><w:tr><w:tc><w:p><w:r><w:t>Ref</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>Qty</w:t></w:r></w:p></w:tc></w:tr>
<w:tr><w:tc><w:p><w:r><w:t>U1</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>1</w:t></w:r></w:p></w:tc></w:tr></w:tbl>
<w:p><w:r><w:drawing><wp:inline><a:graphic><a:graphicData><pic:pic><pic:blipFill><a:blip r:embed="rIdI"/></pic:blipFill></pic:pic></a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p>
<w:p><w:del><w:r><w:t>deleted</w:t></w:r></w:del><w:ins><w:r><w:t>inserted</w:t></w:r></w:ins></w:p>
</w:body></w:document>'''
    num = f'''<?xml version="1.0"?><w:numbering {W}><w:abstractNum w:abstractNumId="0"><w:lvl w:ilvl="0"><w:numFmt w:val="decimal"/></w:lvl></w:abstractNum><w:num w:numId="1"><w:abstractNumId w:val="0"/></w:num></w:numbering>'''
    rels = '''<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rIdL" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink" Target="https://example.com/ds.pdf" TargetMode="External"/><Relationship Id="rIdI" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/image" Target="media/image1.png"/></Relationships>'''
    top = '''<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/></Relationships>'''
    ct = '''<?xml version="1.0"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="xml" ContentType="application/xml"/></Types>'''
    b = io.BytesIO()
    with zipfile.ZipFile(b, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", ct); z.writestr("_rels/.rels", top)
        z.writestr("word/document.xml", doc); z.writestr("word/_rels/document.xml.rels", rels)
        z.writestr("word/numbering.xml", num); z.writestr("word/media/image1.png", PNG)
    return b.getvalue()

def main():
    os.makedirs(OUT, exist_ok=True)
    zb = io.BytesIO()
    with zipfile.ZipFile(zb, "w") as z: z.writestr("board.GTL", "G04 synthetic*\nM02*\n")
    e = EML1.replace("@@HTML@@", b64(HTML1.encode())).replace("@@PNG@@", b64(PNG)).replace("@@CSV@@", b64("Ref,Qty\nR1,2\n".encode())).replace("@@ZIP@@", b64(zb.getvalue()))
    open(os.path.join(OUT, "sample1.eml"), "wb").write(e.replace("\n", "\r\n").encode())
    open(os.path.join(OUT, "sample_1250.eml"), "wb").write(EML2)
    m1, m2 = msg1(), msg2()
    open(os.path.join(OUT, "sample1.msg"), "wb").write(m1)
    open(os.path.join(OUT, "sample2.msg"), "wb").write(m2)
    open(os.path.join(OUT, "sample1.docx"), "wb").write(docx())
    try:
        import olefile
        for n in ("sample1.msg", "sample2.msg"):
            o = olefile.OleFileIO(os.path.join(OUT, n))
            subj = [x for x in o.listdir() if x[-1].startswith("__substg1.0_0037")]
            assert subj, n
            if n == "sample1.msg":
                assert len(o.openstream(["__attach_version1.0_#00000002", "__substg1.0_37010102"]).read()) == 10000
                assert o.openstream(["__attach_version1.0_#00000003", "__substg1.0_3701000D", "__substg1.0_0037001F"]).read().decode("utf-16-le") == "Inner message"
            print("olefile oracle OK:", n, len(o.listdir()), "streams")
    except ImportError:
        print("olefile not installed: CFB oracle check skipped")

if __name__ == "__main__":
    main()
