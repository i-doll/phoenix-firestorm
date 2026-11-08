#!/usr/bin/env python3
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
P = json.load(open(os.path.join(HERE, "palette.json")))
D, Lt = P["dark"], P["light"]
F = 'font-family="DejaVu Sans, sans-serif"'


def floater(x, y, w, h, p, tab_on):
    o = [f'<rect x="{x + 2}" y="{y + 3}" width="{w}" height="{h}" rx="16" fill="#000" fill-opacity="0.35"/>',
         f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="16" fill="{p["cont"]}"/>',
         f'<text x="{x + 14}" y="{y + 22}" {F} font-size="12" fill="{p["onsurface"]}">Preferences</text>',
         f'<path d="M{x + w - 22},{y + 12} l7,7 M{x + w - 15},{y + 12} l-7,7" stroke="{p["onsv"]}" stroke-width="1.5" stroke-linecap="round"/>']
    tx = x + 8
    for i, t in enumerate(["Graphics", "Sound", "Chat", "Theme"]):
        tw = len(t) * 5.1 + 12
        if i == tab_on:
            o.append(f'<rect x="{tx}" y="{y + 32}" width="{tw}" height="20" rx="10" fill="{p["seccont"]}"/>')
        o.append(f'<text x="{tx + 6}" y="{y + 46}" {F} font-size="9" fill="{p["onsec"] if i == tab_on else p["onsv"]}">{t}</text>')
        tx += tw + 3
    cy = y + 60
    o += [f'<rect x="{x + 10}" y="{cy}" width="{w - 20}" height="56" rx="12" fill="{p["high"]}"/>',
          f'<rect x="{x + 20}" y="{cy + 10}" width="12" height="12" rx="2.5" fill="{p["primary"]}"/>',
          f'<path d="M{x + 22.6},{cy + 16.1} l2.4,2.4 l4.6,-5.1" fill="none" stroke="{p["high"]}" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/>',
          f'<text x="{x + 40}" y="{cy + 20}" {F} font-size="10" fill="{p["onsurface"]}">Volumetric clouds</text>',
          f'<rect x="{x + 20.8}" y="{cy + 32.8}" width="10.4" height="10.4" rx="1.8" fill="none" stroke="{p["primary"]}" stroke-width="1.6"/>',
          f'<text x="{x + 40}" y="{cy + 42}" {F} font-size="10" fill="{p["onsurface"]}">Shadows</text>']
    sy = cy + 74
    sx0, sx1 = x + 92, x + w - 14
    sv = sx0 + (sx1 - sx0) * 0.6
    o += [f'<text x="{x + 12}" y="{sy + 4}" {F} font-size="10" fill="{p["onsurface"]}">Draw distance</text>',
          f'<rect x="{sx0}" y="{sy - 2}" width="{sx1 - sx0}" height="4" rx="2" fill="{p["highest"]}"/>',
          f'<rect x="{sx0}" y="{sy - 2}" width="{sv - sx0}" height="4" rx="2" fill="{p["primary"]}"/>',
          f'<circle cx="{sv}" cy="{sy}" r="6.5" fill="{p["primary"]}"/>']
    by = y + h - 32
    o += [f'<rect x="{x + w - 150}" y="{by}" width="66" height="22" rx="11" fill="{p["seccont"]}"/>',
          f'<text x="{x + w - 117}" y="{by + 15}" {F} font-size="10" text-anchor="middle" fill="{p["onsec"]}">Cancel</text>',
          f'<rect x="{x + w - 78}" y="{by}" width="66" height="22" rx="11" fill="{p["primary"]}"/>',
          f'<text x="{x + w - 45}" y="{by + 15}" {F} font-size="10" text-anchor="middle" fill="{p["onprimary"]}">OK</text>']
    return "\n".join(o)


W, H = 414, 225
s = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">',
     '<defs><linearGradient id="g" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#2E5E5B"/><stop offset="1" stop-color="#0B1F1E"/></linearGradient></defs>',
     f'<rect width="{W}" height="{H}" fill="url(#g)"/>',
     f'<rect x="0" y="0" width="{W}" height="16" fill="{D["cont"]}"/>',
     f'<text x="8" y="12" {F} font-size="9" xml:space="preserve" fill="{D["onsurface"]}">Avatar    Comm    World    Build    Content    Help</text>',
     floater(12, 26, 190, 180, Lt, 0),
     floater(214, 26, 190, 180, D, 3),
     f'<rect x="60" y="{H - 16}" width="294" height="14" rx="7" fill="{D["cont"]}"/>']
for i in range(7):
    s.append(f'<rect x="{66 + i * 41}" y="{H - 14}" width="37" height="10" rx="5" fill="{D["primary"] if i == 2 else D["seccont"]}"/>')
s.append("</svg>")
svg_path = os.path.join(sys.argv[2], "preview.svg")
png_path = os.path.join(sys.argv[2], "preview.png")
open(svg_path, "w").write("\n".join(s))
subprocess.run(["resvg", "-w", str(W), "-h", str(H), svg_path, png_path], check=True)
subprocess.run(["magick", png_path, "-quality", "92", sys.argv[1]], check=True)
