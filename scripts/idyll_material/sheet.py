#!/usr/bin/env python3
import json
import os
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
SKIN = sys.argv[1]
manifest = json.load(open(sys.argv[2]))
OUT = sys.argv[3]

PAL = json.load(open(os.path.join(HERE, "palette.json")))


def tint_key(t):
    s = t["tint"]
    if "ButtonSelected" in s:
        return "pc"
    if "ButtonImageColor" in s:
        return "seccont"
    if "EmphasisColor" in s or "thumb_center" in s or "check_box" in s:
        return "primary"
    if "TextBgWriteable" in s or "ScrollbarTrack" in s or "SliderTrack" in s or "color_bg" in s:
        return "highest"
    if "ScrollbarThumb" in s:
        return "outline"
    if "BadgeImage" in s:
        return "error"
    if "BadgeBorder" in s:
        return "surface"
    if "ToolTip" in s:
        return "inverse"
    if "ButtonLabelColor" in s:
        return "onsec"
    if "untinted" in s:
        return None
    if "Floater" in s or "Notify" in s or "Hint" in s or "caller" in s:
        return "high"
    return "primary"


def hexc(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) / 255 for i in (0, 2, 4))


def tinted(img, colour):
    if colour is None:
        return img
    r, g, b = hexc(colour)
    px = img.load()
    out = img.copy()
    po = out.load()
    for y in range(img.height):
        for x in range(img.width):
            pr, pg, pb, pa = px[x, y]
            po[x, y] = (round(pr * r), round(pg * g), round(pb * b), pa)
    return out


def nine(img, scale, W, H):
    w, h = img.size
    if scale is None or tuple(scale) == (0, 0, w, h):
        return img.resize((W, H), Image.BILINEAR)
    l, b, r, t = scale
    left, right = l, w - r
    top, bottom = h - t, b
    shrink = 1.0
    if left + right > W:
        shrink = min(shrink, W / (left + right))
    if top + bottom > H:
        shrink = min(shrink, H / (top + bottom))
    dl, dt = round(left * shrink), round(top * shrink)
    dr, db = min(round(right * shrink), W - dl), min(round(bottom * shrink), H - dt)
    xs_src = [0, left, r, w]
    ys_src = [0, top, h - bottom, h]
    xs_dst = [0, dl, W - dr, W]
    ys_dst = [0, dt, H - db, H]
    out = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    for i in range(3):
        for j in range(3):
            sx0, sx1 = xs_src[i], xs_src[i + 1]
            sy0, sy1 = ys_src[j], ys_src[j + 1]
            dx0, dx1 = xs_dst[i], xs_dst[i + 1]
            dy0, dy1 = ys_dst[j], ys_dst[j + 1]
            if sx1 <= sx0 or sy1 <= sy0 or dx1 <= dx0 or dy1 <= dy0:
                continue
            piece = img.crop((sx0, sy0, sx1, sy1)).resize((dx1 - dx0, dy1 - dy0), Image.BILINEAR)
            out.alpha_composite(piece, (dx0, dy0))
    return out


try:
    font = ImageFont.truetype("/usr/share/fonts/TTF/DejaVuSans.ttf", 10)
except OSError:
    font = ImageFont.load_default()

ROW_H = 64
COLS = 2
items = manifest
rows = (len(items) + COLS - 1) // COLS
cell_w = 2 * 360
sheet = Image.new("RGBA", (cell_w * COLS, rows * ROW_H), "white")
for idx, t in enumerate(items):
    img = Image.open(os.path.join(SKIN, "textures", t["file"])).convert("RGBA")
    col, row = idx % COLS, idx // COLS
    for m, mode in enumerate(("dark", "light")):
        x0 = col * cell_w + m * 360
        y0 = row * ROW_H
        p = PAL[mode]
        bg = Image.new("RGBA", (360, ROW_H), p["surface"])
        d = ImageDraw.Draw(bg)
        d.text((4, 2), t["name"], fill=p["outline"], font=font)
        key = tint_key(t)
        ti = tinted(img, p[key] if key else None)
        fixed = t["scale"] is None and t["w"] <= 16 and t["h"] <= 16
        if fixed:
            sizes = [(t["w"], t["h"]), (t["w"] * 2, t["h"] * 2)]
            sizes = [(t["w"], t["h"])]
            bg.alpha_composite(ti, (8, 20))
            big = ti.resize((t["w"] * 3, t["h"] * 3), Image.NEAREST)
            bg.alpha_composite(big, (40, 16))
        else:
            sizes = [(max(8, round(t["w"] * 0.75)), max(4, round(t["h"] * 0.75))), (t["w"] * 2, t["h"]),
                     (min(170, t["w"] * 4), min(46, max(t["h"] * 2, t["h"])))]
            if t["h"] <= 6:
                sizes = [(20, t["h"]), (80, t["h"]), (200, t["h"])]
            if t["w"] <= 6:
                sizes = [(t["w"], 20), (t["w"], 40), (t["w"], 46)]
            x = 6
            for (W, H) in sizes:
                bg.alpha_composite(nine(ti, t["scale"], W, H), (x, 16))
                x += W + 10
        sheet.alpha_composite(bg, (x0, y0))
sheet.convert("RGB").save(OUT)
print(OUT, sheet.size)
