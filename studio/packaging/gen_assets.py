#!/usr/bin/env python3
"""Regenerates the Studio icon and the DMG background (committed outputs; CI does not need Pillow).
   python3 studio/packaging/gen_assets.py"""
import math, os
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
S = 4  # supersampling

DY = -85  # lifts the silhouette (no leaf) so it sits centred on the canvas
SC = 0.9  # silhouette scale about the canvas centre
T = lambda v: 512 + (v - 512) * SC

def apple_shape(W, k):
    """The original apple silhouette (two lobes, top dip, no bite, no leaf) on a 1024 grid, as an L mask of side W."""
    m = Image.new("L", (W, W), 0); d = ImageDraw.Draw(m)
    def circ(cx, cy, r, v=255): X, Y, R = T(cx), T(cy + DY), r * SC; d.ellipse([(X - R) * k, (Y - R) * k, (X + R) * k, (Y + R) * k], fill=v)
    circ(420, 560, 215); circ(604, 560, 215)
    d.ellipse([T(512 - 290) * k, T(610 + DY - 230) * k, T(512 + 290) * k, T(610 + DY + 235) * k], fill=255)
    circ(440, 690, 170); circ(584, 690, 170)
    circ(512, 332, 62, 0)            # top dip
    return m

def joycon_apple(size, gap=16):
    """Two-tone 'Joy-Con' apple: a black left half with a stick, a white right half with four buttons. Returns (black, white) RGBA layers."""
    k = size / 1024 * S; W = size * S
    def layer(half, ink, face, detail):
        shape = apple_shape(W, k)
        cut = Image.new("L", (W, W), 0); cd = ImageDraw.Draw(cut)
        cd.rectangle([0, 0, (512 - gap / 2) * k, W] if half == 0 else [(512 + gap / 2) * k, 0, W, W], fill=255)
        shape = ImageChops.multiply(shape, cut)
        im = Image.new("RGBA", (W, W), (0, 0, 0, 0)); im.paste(face, (0, 0), shape)
        d = ImageDraw.Draw(im); c = lambda x, y, r: d.ellipse([(x - r) * k, (y - r) * k, (x + r) * k, (y + r) * k], fill=ink)
        for (x, y, r) in detail: c(T(x), T(y), r * SC)
        return im.resize((size, size), Image.LANCZOS)
    left = layer(0, (255, 255, 255, 255), (22, 22, 26, 255), [(385, 490, 60)])
    right = layer(1, (22, 22, 26, 255), (255, 255, 255, 255), [(645, 440, 26), (645, 550, 26), (590, 495, 26), (700, 495, 26)])
    return left, right

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
    left, right = joycon_apple(size)
    both = Image.alpha_composite(left, right)
    sh = both.getchannel("A").transform(both.size, Image.AFFINE, (1, 0, 0, 0, 1, -size * 0.016))
    sh = sh.filter(ImageFilter.GaussianBlur(size * 0.012)).point(lambda v: int(v * 0.35))
    shadow = Image.new("RGBA", out.size, (0, 0, 0, 0)); shadow.putalpha(sh)
    return Image.alpha_composite(Image.alpha_composite(out, shadow), both)

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
    big = icon(1024)
    big.save(os.path.join(HERE, "icon.png"))
    site = os.path.join(HERE, "..", "..", "site", "assets")
    for name, px in (("icon.png", 512), ("apple-touch-icon.png", 180), ("favicon.png", 64)):
        big.resize((px, px), Image.LANCZOS).save(os.path.join(site, name))
    dmg_bg(1).save(os.path.join(HERE, "dmg-background.png"))
    dmg_bg(2).save(os.path.join(HERE, "dmg-background@2x.png"))
    print("ok")
