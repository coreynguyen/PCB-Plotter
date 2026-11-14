#!/usr/bin/env python3
# Generates the Explorer file-type icons res/icons/*.ico (16/20/24/32/40/48/64/256, PNG-compressed 256)
# and a contact sheet. Style matches res/app.ico (tools/make_icon.py): green board, gold traces,
# white crosshair — placed on a document page with a coloured type badge.
#   python tools/make_icons.py [contact_sheet.png]
# Resource ids (res/app.rc, src/platform/win_shell.h) are stable: never renumber.
import os, sys
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "res", "icons")
SIZES = [16, 20, 24, 32, 40, 48, 64, 256]

# id, file, badge text, badge colour, short text for <= 24 px
TYPES = [
    (101, "pcbp",   "PCBP", (31, 122, 77),  "P"),
    (102, "cpf",    "CPF",  (196, 120, 20), "C"),
    (103, "vmjob",  "VM",   (118, 72, 170), "V"),
    (104, "gerber", "GBR",  (190, 50, 50),  "G"),
    (105, "odb",    "ODB",  (0, 128, 128),  "O"),
    (106, "ipc",    "IPC",  (40, 90, 180),  "I"),
    (107, "pnp",    "PNP",  (220, 95, 20),  "J"),
    (108, "sheet",  "XLS",  (24, 110, 60),  "X"),
]
IMPORT_ID = 120  # context-menu verb icon (board + arrow)

FONTS = ["/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed-Bold.ttf",
         "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
         "C:/Windows/Fonts/arialbd.ttf", "C:/Windows/Fonts/segoeuib.ttf",
         "/System/Library/Fonts/Supplemental/Arial Bold.ttf"]

def font(px):
    for f in FONTS:
        if os.path.exists(f):
            return ImageFont.truetype(f, px)
    try:
        return ImageFont.load_default(px)
    except TypeError:
        return ImageFont.load_default()

GOLD = (232, 185, 49, 255)
BOARD = (31, 122, 77, 255)
BOARD_EDGE = (13, 59, 36, 255)
WHITE = (255, 255, 255, 255)

def board(d, x0, y0, s, u):
    """mini PCB in the square (x0,y0,s) — same motif as the app icon."""
    def P(x, y): return (x0 + x * s / 16.0, y0 + y * s / 16.0)
    k = s / 16.0
    d.rounded_rectangle([P(0.5, 0.5), P(15.5, 15.5)], radius=2.6 * k, fill=BOARD, outline=BOARD_EDGE, width=max(1, int(0.6 * k)))
    w = max(1, int(1.1 * k))
    d.line([P(3.5, 4), P(8, 4), P(12, 8)], fill=GOLD, width=w, joint="curve")
    d.line([P(3.5, 12), P(7, 12)], fill=GOLD, width=w)
    for cx, cy in ((3.5, 4), (3.5, 12)):
        d.ellipse([P(cx - 1.7, cy - 1.7), P(cx + 1.7, cy + 1.7)], fill=GOLD)
    d.ellipse([P(8, 8), P(14, 14)], outline=WHITE, width=max(1, int(0.9 * k)))
    d.line([P(11, 6.8), P(11, 15.2)], fill=WHITE, width=max(1, int(0.7 * k)))
    d.line([P(6.8, 11), P(15.2, 11)], fill=WHITE, width=max(1, int(0.7 * k)))

def render(n, text, col, short):
    S = 8 if n <= 64 else 4
    W = n * S
    im = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    u = W / 32.0
    small = n <= 24
    # document page with folded corner
    x0, y0, x1, y1 = (3 * u, 1 * u, 29 * u, 31 * u) if not small else (2 * u, 0.5 * u, 30 * u, 31.5 * u)
    f = 7 * u
    page = [(x0, y0), (x1 - f, y0), (x1, y0 + f), (x1, y1), (x0, y1)]
    d.polygon(page, fill=(250, 250, 248, 255))
    d.line(page + [page[0]], fill=(120, 124, 130, 255), width=max(1, int(1.0 * u)), joint="curve")
    d.polygon([(x1 - f, y0), (x1 - f, y0 + f), (x1, y0 + f)], fill=(214, 218, 222, 255), outline=(120, 124, 130, 255))
    # board
    bs = 15.5 * u if not small else 17 * u
    bx = (x0 + x1) / 2 - bs / 2 - (1.0 * u if not small else 0.5 * u)
    by = y0 + (3.5 * u if not small else 2.0 * u)
    board(d, bx, by, bs, u)
    # badge
    by0 = y1 - (10.5 * u if not small else 11.5 * u)
    bx0, bx1, by1 = x0 - (1.5 * u if not small else 1.0 * u), x1 + (0 if not small else 0.5 * u), y1 - 0.8 * u
    d.rounded_rectangle([bx0, by0, bx1, by1], radius=1.8 * u, fill=col + (255,))
    label = short if small else text
    target_h = (by1 - by0) * (0.78 if small else 0.66)
    fs = int(target_h * 1.25)
    fnt = font(fs)
    while fs > 6:
        bb = d.textbbox((0, 0), label, font=fnt)
        if bb[2] - bb[0] <= (bx1 - bx0) * 0.86 and bb[3] - bb[1] <= target_h: break
        fs -= max(1, fs // 20); fnt = font(fs)
    bb = d.textbbox((0, 0), label, font=fnt)
    tx = (bx0 + bx1) / 2 - (bb[0] + bb[2]) / 2
    ty = (by0 + by1) / 2 - (bb[1] + bb[3]) / 2
    d.text((tx, ty), label, font=fnt, fill=WHITE)
    return im.resize((n, n), Image.LANCZOS)

def render_import(n):
    S = 8
    W = n * S
    im = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    u = W / 16.0
    board(d, 3 * u, 3 * u, 13 * u, u)
    # arrow into the board (from the top-left corner), blue with a white rim
    blue, rim = (40, 110, 210, 255), WHITE
    for col, grow in ((rim, 1.0 * u), (blue, 0)):
        d.line([(1.2 * u, 1.2 * u), (5.0 * u, 5.0 * u)], fill=col, width=int(2.2 * u + 2 * grow))
        d.polygon([(7.6 * u + grow, 7.6 * u + grow), (7.6 * u + grow, 2.6 * u - grow), (2.6 * u - grow, 7.6 * u + grow)], fill=col)
    return im.resize((n, n), Image.LANCZOS)

def save_ico(path, imgs):
    imgs = sorted(imgs, key=lambda i: i.size[0])
    imgs[-1].save(path, format="ICO", sizes=[i.size for i in imgs], append_images=imgs[:-1])

def main():
    os.makedirs(OUT, exist_ok=True)
    sheet_rows = []
    for rid, name, text, col, short in TYPES:
        imgs = [render(s, text, col, short) for s in SIZES]
        save_ico(os.path.join(OUT, name + ".ico"), imgs)
        sheet_rows.append((name, imgs))
    imp = [render_import(s) for s in (16, 20, 24, 32, 48, 256)]
    save_ico(os.path.join(OUT, "import.ico"), imp)
    sheet_rows.append(("import", imp))
    if len(sys.argv) > 1:
        pad = 12
        colw = [s + pad for s in SIZES]
        W = 110 + sum(colw)
        H = sum(max(i.size[1] for i in r[1]) + pad for r in sheet_rows) + pad
        for bg_name, bg in (("light", (243, 243, 243, 255)), ("dark", (32, 32, 32, 255))):
            pass
        sheet = Image.new("RGBA", (W * 2, H), (243, 243, 243, 255))
        dd = ImageDraw.Draw(sheet)
        dd.rectangle([W, 0, 2 * W, H], fill=(32, 32, 32, 255))
        lf = font(14)
        for half, tc in ((0, (20, 20, 20, 255)), (W, (230, 230, 230, 255))):
            y = pad
            for name, imgs in sheet_rows:
                rh = max(i.size[1] for i in imgs)
                dd.text((half + 8, y + rh / 2 - 8), name, font=lf, fill=tc)
                x = half + 110
                for i, s in zip(imgs, SIZES):
                    sheet.alpha_composite(i, (int(x), int(y + rh - i.size[1])))
                    x += i.size[0] + pad
                y += rh + pad
        sheet.save(sys.argv[1])
    print("icons:", ", ".join(t[1] for t in TYPES) + ", import")

if __name__ == "__main__":
    main()
