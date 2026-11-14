# Generates res/app.ico (multi-size) — PCB board with gold traces + plotter crosshair.
import sys
from PIL import Image, ImageDraw
def render(n):
    S = 8  # supersample
    W = n * S
    im = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    u = W / 16.0
    def P(x, y): return (x * u, y * u)
    d.rounded_rectangle([P(0.5, 0.5), P(15.5, 15.5)], radius=3 * u, fill=(31, 122, 77, 255), outline=(13, 59, 36, 255), width=max(1, int(0.6 * u)))
    gold = (232, 185, 49, 255)
    w = max(1, int(1.0 * u))
    d.line([P(3.5, 4), P(8, 4), P(12, 8)], fill=gold, width=w, joint="curve")
    d.line([P(3.5, 12), P(7, 12)], fill=gold, width=w)
    for cx, cy in ((3.5, 4), (3.5, 12)):
        d.ellipse([P(cx - 1.6, cy - 1.6), P(cx + 1.6, cy + 1.6)], fill=gold)
    white = (255, 255, 255, 255)
    d.ellipse([P(8, 8), P(14, 14)], outline=white, width=max(1, int(0.9 * u)))
    d.line([P(11, 6.8), P(11, 15.2)], fill=white, width=max(1, int(0.7 * u)))
    d.line([P(6.8, 11), P(15.2, 11)], fill=white, width=max(1, int(0.7 * u)))
    return im.resize((n, n), Image.LANCZOS)
sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
imgs = [render(s) for s in sizes]
imgs[-1].save(sys.argv[1], format="ICO", sizes=[(s, s) for s in sizes], append_images=imgs[:-1])
imgs[-1].save(sys.argv[1].replace(".ico", "_256.png"))
print("ok")
