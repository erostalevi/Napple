#!/usr/bin/env python3
"""Regenerates the Studio icon and the DMG background (committed outputs; CI does not need Pillow).
   python3 studio/packaging/gen_assets.py"""
import math, os
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
S = 4  # supersampling

def apple_mask(size):
    """White apple with a bite and a leaf on a transparent canvas, drawn on a 1024 grid scaled to `size`."""
    k = size / 1024 * S
    m = Image.new("L", (int(size * S / (size / 1024) * (size / 1024)) , int(size * S / (size / 1024) * (size / 1024))), 0) if False else Image.new("L", (int(size * S), int(size * S)), 0)
    d = ImageDraw.Draw(m)
    def circ(cx, cy, r, v=255): d.ellipse([(cx - r) * k, (cy - r) * k, (cx + r) * k, (cy + r) * k], fill=v)
    circ(420, 560, 215); circ(604, 560, 215)
    d.ellipse([(512 - 290) * k, (610 - 230) * k, (512 + 290) * k, (610 + 235) * k], fill=255)
    circ(440, 690, 170); circ(584, 690, 170)
    circ(512, 372, 70, 0)            # top dip
    circ(790, 470, 105, 0)           # the bite
    # leaf
    pts = []
    for i in range(41):
        t = i / 40
        x = 170 * t; y = 62 * math.sin(math.pi * t)
        pts.append((x, y))
    leaf = [(x, y) for x, y in pts] + [(x, -y) for x, y in reversed(pts)]
    a = math.radians(-50)
    L = [((520 + (x * math.cos(a) - y * math.sin(a))) * k, (345 + (x * math.sin(a) + y * math.cos(a))) * k) for x, y in leaf]
    d.polygon(L, fill=255)
    return m.resize((size, size), Image.LANCZOS)

def icon(size=1024):
    bg = Image.new("RGB", (size, size))
    px = bg.load()
    for y in range(size):
        for x in range(size):
            t = (x * 0.35 + y * 0.65) / size
            px[x, y] = (int(255 - 40 * t), int(94 - 54 * t), int(98 - 28 * t) + int(70 * max(0, t - 0.6)))
    out = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    r = int(size * 0.2237)
    mask = Image.new("L", (size * S, size * S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([int(size * 0.098 * S), int(size * 0.098 * S), int(size * 0.902 * S), int(size * 0.902 * S)], radius=int(size * 0.18 * S), fill=255)
    mask = mask.resize((size, size), Image.LANCZOS)
    out.paste(bg, (0, 0), mask)
    ap = apple_mask(size)
    scale = 0.64; a2 = ap.resize((int(size * scale), int(size * scale)), Image.LANCZOS)
    shadow = Image.new("RGBA", out.size, (0, 0, 0, 0))
    sh = Image.new("L", out.size, 0); sh.paste(a2, (int(size * (1 - scale) / 2), int(size * (1 - scale) / 2) + int(size * 0.02)))
    sh = sh.filter(ImageFilter.GaussianBlur(size * 0.012)).point(lambda v: int(v * 0.35))
    shadow.putalpha(sh); out = Image.alpha_composite(out, shadow)
    white = Image.new("RGBA", out.size, (0, 0, 0, 0))
    wm = Image.new("L", out.size, 0); wm.paste(a2, (int(size * (1 - scale) / 2), int(size * (1 - scale) / 2)))
    white.paste((255, 255, 255, 255), (0, 0), wm)
    return Image.alpha_composite(out, white)

def font(sz, bold=False):
    for p in ["/System/Library/Fonts/SFNS.ttf", "/usr/share/fonts/opentype/inter/Inter-%s.otf" % ("Bold" if bold else "Regular"),
              "/usr/share/fonts/truetype/dejavu/DejaVuSans%s.ttf" % ("-Bold" if bold else "")]:
        if os.path.exists(p): return ImageFont.truetype(p, sz)
    return ImageFont.load_default()

def dmg_bg(scale):
    W, H = 660 * scale, 400 * scale
    im = Image.new("RGB", (W, H))
    px = im.load()
    for y in range(H):
        for x in range(W):
            t = y / H
            px[x, y] = (int(22 + 14 * t), int(14 + 6 * t), int(30 + 22 * t))
    glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    gd.ellipse([W * 0.55, -H * 0.5, W * 1.15, H * 0.6], fill=(255, 80, 90, 70))
    gd.ellipse([-W * 0.2, H * 0.55, W * 0.4, H * 1.3], fill=(120, 80, 255, 50))
    glow = glow.filter(ImageFilter.GaussianBlur(60 * scale))
    im = Image.alpha_composite(im.convert("RGBA"), glow)
    d = ImageDraw.Draw(im)
    title = font(22 * scale, True); sub = font(13 * scale)
    d.text((W / 2, 52 * scale), "Install Napple Studio", font=title, fill=(255, 255, 255, 255), anchor="mm")
    d.text((W / 2, 82 * scale), "Drag the app onto Applications. That's the whole setup.", font=sub, fill=(255, 255, 255, 150), anchor="mm")
    # arrow between the two icon slots (180,190) -> (480,190)
    y = 190 * scale; x0, x1 = 262 * scale, 398 * scale
    d.line([(x0, y), (x1, y)], fill=(255, 255, 255, 170), width=int(4 * scale))
    d.polygon([(x1 + 4 * scale, y), (x1 - 14 * scale, y - 12 * scale), (x1 - 14 * scale, y + 12 * scale)], fill=(255, 255, 255, 170))
    d.text((W / 2, 352 * scale), "On first launch Studio sets up the napple engine and the NAPPLE_HOME / NAPPLE_CLI variables for you.",
           font=font(11 * scale), fill=(255, 255, 255, 110), anchor="mm")
    return im.convert("RGB")

if __name__ == "__main__":
    icon(1024).save(os.path.join(HERE, "icon.png"))
    dmg_bg(1).save(os.path.join(HERE, "dmg-background.png"))
    dmg_bg(2).save(os.path.join(HERE, "dmg-background@2x.png"))
    print("ok")
