#!/usr/bin/env python3
import json
import os
import subprocess
import sys

SKIN = sys.argv[1]
WORK = sys.argv[3]
OUT_SVG = os.path.join(WORK, "svg")
os.makedirs(OUT_SVG, exist_ok=True)

STROKE = 1.6
PRESS = 0.80
DISABLED = 0.45
RING = 0.75


def rrect(x, y, w, h, r):
    if isinstance(r, (int, float)):
        r = (r, r, r, r)
    tl, tr, br, bl = [min(v, w / 2, h / 2) for v in r]
    return (f"M{x + tl},{y} H{x + w - tr} "
            f"{'A%s,%s 0 0 1 %s,%s' % (tr, tr, x + w, y + tr) if tr else ''} V{y + h - br} "
            f"{'A%s,%s 0 0 1 %s,%s' % (br, br, x + w - br, y + h) if br else ''} H{x + bl} "
            f"{'A%s,%s 0 0 1 %s,%s' % (bl, bl, x, y + h - bl) if bl else ''} V{y + tl} "
            f"{'A%s,%s 0 0 1 %s,%s' % (tl, tl, x + tl, y) if tl else ''} Z")


def grey(v, a=1.0):
    c = round(v * 255)
    return f'fill="rgb({c},{c},{c})" fill-opacity="{a}"'


def svg(w, h, body, defs=""):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}">'
            f'<defs>{defs}</defs>{body}</svg>')


def shape(w, h, r, inset=(0, 0, 0, 0)):
    l, t, rr, b = inset
    return rrect(l, t, w - l - rr, h - t - b, r)


def filled(w, h, r, value=1.0, alpha=1.0, inset=(0, 0, 0, 0)):
    return svg(w, h, f'<path d="{shape(w, h, r, inset)}" {grey(value, alpha)}/>')


def ring_path(w, h, r, width, inset=(0, 0, 0, 0)):
    l, t, rr, b = inset
    outer = shape(w, h, r, inset)
    rin = tuple(max(0, v - width) for v in (r if isinstance(r, tuple) else (r,) * 4))
    inner = rrect(l + width, t + width, w - l - rr - 2 * width, h - t - b - 2 * width, rin)
    return f'<path d="{outer} {inner}" fill-rule="evenodd"'


def selected(w, h, r, value=1.0, alpha=1.0, inset=(0, 0, 0, 0), ring=1.5, gap=1.5):
    l, t, rr, b = inset
    rt = r if isinstance(r, tuple) else (r,) * 4
    d = ring + gap
    rin = tuple(max(0, v - d) if v else 0 for v in rt)
    inner = rrect(l + d, t + d, w - l - rr - 2 * d, h - t - b - 2 * d, rin)
    body = (f'{ring_path(w, h, rt, ring, inset)} {grey(RING, alpha)}/>'
            f'<path d="{inner}" {grey(value, alpha)}/>')
    return svg(w, h, body)


def empty(w, h):
    return svg(w, h, "")


def chevron(cx, cy, size, direction, sw=1.6):
    s = size / 2
    q = s / 2
    pts = {
        "down": [(cx - s, cy - q), (cx, cy + q), (cx + s, cy - q)],
        "up": [(cx - s, cy + q), (cx, cy - q), (cx + s, cy + q)],
        "left": [(cx + q, cy - s), (cx - q, cy), (cx + q, cy + s)],
        "right": [(cx - q, cy - s), (cx + q, cy), (cx - q, cy + s)],
    }[direction]
    d = "M" + " L".join(f"{x},{y}" for x, y in pts)
    return (f'<path d="{d}" fill="none" stroke-width="{sw}" stroke-linecap="round" '
            f'stroke-linejoin="round"')


def glyph(w, h, direction, size=7):
    return svg(w, h, f'{chevron(w / 2, h / 2, size, direction)} stroke="white"/>')


def checkbox(w, h, box, on, press=False, alpha=1.0):
    x = (w - box) / 2
    y = (h - box) / 2
    r = 2.5
    if not on:
        body = f'{ring_path(w, h, r, STROKE, (x, y, w - x - box, h - y - box))} {grey(1, alpha)}/>'
        if press:
            body += f'<path d="{rrect(x, y, box, box, r)}" {grey(1, 0.3 * alpha)}/>'
        return svg(w, h, body)
    cx, cy = x + box / 2, y + box / 2
    check = f"M{cx - 3.4},{cy + 0.1} L{cx - 1},{cy + 2.5} L{cx + 3.6},{cy - 2.6}"
    defs = (f'<mask id="m"><rect width="{w}" height="{h}" fill="white"/>'
            f'<path d="{check}" fill="none" stroke="black" stroke-width="1.8" stroke-linecap="round" '
            f'stroke-linejoin="round"/></mask>')
    body = f'<path d="{rrect(x, y, box, box, r)}" {grey(PRESS if press else 1, alpha)} mask="url(#m)"/>'
    return svg(w, h, body, defs)


def radio(w, h, on, press=False, alpha=1.0):
    cx, cy, r = w / 2, h / 2, 6
    body = (f'<circle cx="{cx}" cy="{cy}" r="{r - STROKE / 2}" fill="none" stroke="white" '
            f'stroke-width="{STROKE}" stroke-opacity="{alpha}"/>')
    if on:
        body += f'<circle cx="{cx}" cy="{cy}" r="3" {grey(1, alpha)}/>'
    if press:
        body += f'<circle cx="{cx}" cy="{cy}" r="{r - STROKE}" {grey(1, 0.3 * alpha)}/>'
    return svg(w, h, body)


def thumb(w, h, kind):
    cx, cy = w / 2, h / 2
    if kind == "off":
        return svg(w, h, f'<circle cx="{cx}" cy="{cy}" r="6.5" {grey(1)}/>')
    if kind == "press":
        return svg(w, h, f'<circle cx="{cx}" cy="{cy}" r="7" {grey(1, 0.3)}/>'
                         f'<circle cx="{cx}" cy="{cy}" r="5" {grey(1)}/>')
    return svg(w, h, f'<circle cx="{cx}" cy="{cy}" r="5.5" {grey(1, DISABLED)}/>')


def hline(w, h, alpha):
    return svg(w, h, f'<rect x="0" y="{(h - 1) / 2}" width="{w}" height="1" fill="rgb(128,128,128)" fill-opacity="{alpha}"/>')


def stepper(w, h, direction, value=1.0):
    r = (4, 4, 0, 0) if direction == "up" else (0, 0, 4, 4)
    inset = (0, 0.5, 0, 0) if direction == "up" else (0, 0, 0, 0.5)
    return svg(w, h, f'<path d="{shape(w, h, r, inset)}" {grey(value)}/>')


T = []
BTN_TINT = "ButtonImageColor (SecondaryContainer)"
BTN_SEL_TINT = "image_color_selected ButtonSelectedBgColor (PrimaryContainer) when toggled on, else ButtonImageColor"


def add(name, file_name, w, h, content, scale=None, tint="", old=None, entry_file=False):
    if tint == BTN_TINT and ("Selected" in name or name.endswith("_On") or "Flash" in name):
        tint = BTN_SEL_TINT
    T.append(dict(name=name, file=file_name, w=w, h=h, svg=content, scale=scale, tint=tint, entry_file=entry_file))


PILL = (41, 25, 12, (12, 12, 29, 13))
pw, ph, pr, ps = PILL
BTN = BTN_TINT
for n, c in [
    ("PushButton_Off", filled(pw, ph, pr)),
    ("PushButton_Over", filled(pw, ph, pr)),
    ("PushButton_Press", filled(pw, ph, pr, PRESS)),
    ("PushButton_Disabled", filled(pw, ph, pr, 1, DISABLED)),
    ("PushButton_Selected", selected(pw, ph, pr)),
    ("PushButton_On", selected(pw, ph, pr)),
    ("PushButton_On_Selected", selected(pw, ph, pr)),
    ("PushButton_Selected_Press", selected(pw, ph, pr, PRESS)),
    ("PushButton_Selected_Disabled", selected(pw, ph, pr, 1, DISABLED)),
]:
    add(n, f"widgets/{n}.png", pw, ph, c, ps, BTN)

SEG_W = 41
L = (12, 0, 0, 12)
R = (0, 12, 12, 0)
M = (3, 3, 3, 3)
seg_scale = {"Left": (12, 12, 28, 13), "Right": (13, 12, 29, 13), "Middle": (4, 4, 37, 21)}
seg_r = {"Left": L, "Right": R, "Middle": M}
seg_inset = {"Left": (0, 0, 1, 0), "Right": (1, 0, 0, 0), "Middle": (0, 0, 0, 0)}
for side in ("Left", "Right", "Middle"):
    r = seg_r[side]
    i = seg_inset[side]
    states = {
        "Off": filled(SEG_W, 25, r, inset=i),
        "Over": filled(SEG_W, 25, r, inset=i),
        "Press": filled(SEG_W, 25, r, PRESS, inset=i),
        "Disabled": filled(SEG_W, 25, r, 1, DISABLED, inset=i),
        "Selected": selected(SEG_W, 25, r, inset=i),
        "Selected_Over": selected(SEG_W, 25, r, inset=i),
        "Selected_Press": selected(SEG_W, 25, r, PRESS, inset=i),
        "Selected_Disabled": selected(SEG_W, 25, r, 1, DISABLED, inset=i),
    }
    if side == "Middle":
        states = {
            "Disabled": filled(SEG_W, 25, r, 1, DISABLED),
            "Selected": filled(SEG_W, 25, r),
            "Selected_Press": filled(SEG_W, 25, r, PRESS),
            "Selected_Disabled": filled(SEG_W, 25, r, 1, DISABLED),
        }
    if side == "Right":
        states.pop("Selected_Over")
    for st, c in states.items():
        n = f"SegmentedBtn_{side}_{st}"
        add(n, f"widgets/{n}.png", SEG_W, 25, c, seg_scale[side], BTN)

CB_R = (0, 12, 12, 0)
CBW = 22
for st, c in {
    "Off": filled(CBW, 25, CB_R, inset=(1, 0, 0, 0)),
    "On": filled(CBW, 25, CB_R, inset=(1, 0, 0, 0)),
    "Selected": filled(CBW, 25, CB_R, PRESS, inset=(1, 0, 0, 0)),
    "Disabled": filled(CBW, 25, CB_R, 1, DISABLED, inset=(1, 0, 0, 0)),
}.items():
    n = f"ComboButton_{st}"
    add(n, f"widgets/{n}.png", CBW, 25, c, (1, 12, 10, 13), BTN)
for st, v in (("UpOff", 1.0), ("UpSelected", PRESS)):
    n = f"ComboButton_{st}"
    add(n, f"widgets/{n}.png", CBW, 25, filled(CBW, 25, CB_R, v, inset=(1, 0, 0, 0)), (1, 12, 10, 13), BTN)

for st, c in {
    "Off": filled(pw, ph, pr),
    "On": filled(pw, ph, pr),
    "Press": filled(pw, ph, pr, PRESS),
    "Disabled": filled(pw, ph, pr, 1, DISABLED),
}.items():
    n = f"DropDown_{st}"
    add(n, f"widgets/{n}.png", pw, ph, c, ps, BTN)
add("Combobox_Over", "widgets/Combobox_Over.png", 12, 12, glyph(12, 12, "down", 8), None,
    "image_overlay_color ButtonLabelColor (OnSecondaryContainer)", entry_file=True)
add("IDChevron_Up", "widgets/IDChevron_Up.png", 12, 12, glyph(12, 12, "up", 8), None,
    "image_overlay_color ButtonLabelColor (OnSecondaryContainer)", entry_file=True)

TF = (41, 25, 8, (8, 8, 33, 17))
TFS = (41, 25, 12, (12, 12, 29, 13))
LE = "bg_image_color TextBgWriteableColor (SurfaceContainerHighest)"
for prefix, (w, h, r, sc) in (("TextField", TF), ("TextField_Search", TFS)):
    for st, c in {
        "Off": filled(w, h, r),
        "Active": filled(w, h, r),
        "Disabled": filled(w, h, r, 1, 0.5),
        "Highlight": svg(w, h, f'<path d="{shape(w, h, r)}" {grey(1)}/>{ring_path(w, h, r, 1.5)} {grey(0.55)}/>'),
    }.items():
        n = f"{prefix}_{st}"
        add(n, f"widgets/{n}.png", w, h, c, sc, LE)

CHK = "image_color EmphasisColor (Primary); disabled LabelDisabledColor"
for n, f, c in [
    ("Checkbox_Off", "Checkbox_Off", checkbox(14, 15, 12, False)),
    ("Checkbox_Press", "Checkbox_Press", checkbox(14, 15, 12, False, True)),
    ("Checkbox_On", "Checkbox_On", checkbox(14, 15, 12, True)),
    ("Checkbox_On_Press", "Checkbox_On_Press", checkbox(14, 15, 12, True, True)),
    ("Checkbox_Off_Disabled", "Checkbox_Disabled", checkbox(14, 15, 12, False)),
    ("Checkbox_On_Disabled", "Checkbox_On_Disabled", checkbox(14, 15, 12, True)),
]:
    add(n, f"widgets/{f}.png", 14, 15, c, None, CHK)
for n, on in (("Checkbox_Slim_Off", False), ("Checkbox_Slim_On", True)):
    add(n, f"widgets/{n}.png", 14, 14, checkbox(14, 14, 12, on), None, "image_color from check_box.xml defaults")
for n, f, c in [
    ("RadioButton_Off", "RadioButton_Off", radio(14, 15, False)),
    ("RadioButton_Press", "RadioButton_Press", radio(14, 15, False, True)),
    ("RadioButton_On", "RadioButton_On", radio(14, 15, True)),
    ("RadioButton_On_Press", "RadioButton_On_Press", radio(14, 15, True, True)),
    ("RadioButton_Disabled", "RadioButton_Disabled", radio(14, 15, False)),
    ("RadioButton_On_Disabled", "RadioButton_On_Disabled", radio(14, 15, True)),
]:
    add(n, f"widgets/{f}.png", 14, 15, c, None, CHK)

TAB = (45, 25, 12, (14, 12, 31, 13), (2, 0, 2, 0))
tw, th, tr, ts, ti = TAB
for prefix, names in (
    ("TabTop", ["Left", "Middle", "Right", "First_Flat", "Middle_Flat", "Last_Flat"]),
    ("Toolbar", ["Left", "Middle", "Right"]),
):
    for part in names:
        folder = "containers"
        states = {"Off": empty(tw, th), "Selected": filled(tw, th, tr, inset=ti)}
        if prefix == "Toolbar":
            states["Over"] = filled(tw, th, tr, 1, 0.4, inset=ti)
            states["Flash"] = filled(tw, th, tr, inset=ti)
            states["IM_Unselected"] = empty(tw, th)
        if part == "Right" and prefix == "TabTop":
            states["Flashing"] = filled(tw, th, tr, inset=ti)
        for st, c in states.items():
            n = f"{prefix}_{part}_{st}"
            add(n, f"{folder}/{n}.png", tw, th, c, ts, BTN)
for st, c in {"Off": empty(tw, th), "Selected": filled(tw, th, tr, inset=ti)}.items():
    n = f"TabLeft_Flat_{st}"
    add(n, f"containers/{n}.png", tw, th, c, ts, BTN)

add("SliderTrack_Horiz", "widgets/SliderTrack_Horiz.png", 32, 4, filled(32, 4, 2), (2, 1, 30, 3),
    "track_color SliderTrackColor; highlight track_highlight_color SliderThumbCenterColor")
add("SliderTrack_Vert", "widgets/SliderTrack_Vert.png", 4, 32, filled(4, 32, 2), (1, 2, 3, 30),
    "track_color SliderTrackColor")
for n, k in (("SliderThumb_Off", "off"), ("SliderThumb_Press", "press"), ("SliderThumb_Disabled", "disabled")):
    add(n, f"widgets/{n}.png", 14, 14, thumb(14, 14, k), None,
        "thumb_center_color / thumb_outline_color (Primary)")

add("ScrollThumb_Vert", "widgets/ScrollThumb_Vert.png", 15, 40, filled(15, 40, 3.5, inset=(4, 2, 4, 2)), (7, 6, 8, 34),
    "thumb_color ScrollbarThumbColor (Outline)")
add("ScrollThumb_Horiz", "widgets/ScrollThumb_Horiz.png", 40, 15, filled(40, 15, 3.5, inset=(2, 4, 2, 4)), (6, 7, 34, 8),
    "thumb_color ScrollbarThumbColor (Outline)")
add("ScrollTrack_Vert", "widgets/ScrollTrack_Vert.png", 15, 40, filled(15, 40, 3.5, inset=(4, 2, 4, 2)), (7, 6, 8, 34),
    "track_color ScrollbarTrackColor (SurfaceContainerHighest)")
add("ScrollTrack_Horiz", "widgets/ScrollTrack_Horiz.png", 40, 15, filled(40, 15, 3.5, inset=(2, 4, 2, 4)), (6, 7, 34, 8),
    "track_color ScrollbarTrackColor (SurfaceContainerHighest)")
for d in ("Up", "Down", "Left", "Right"):
    add(f"ScrollArrow_{d}", f"widgets/ScrollArrow_{d}.png", 15, 15, glyph(15, 15, d.lower(), 7), (0, 0, 15, 15),
        "image_color ScrollbarThumbColor (Outline)")

for d in ("Up", "Down"):
    add(f"Stepper_{d}_Off", f"widgets/Stepper_{d}_Off.png", 16, 11, stepper(16, 11, d.lower()), None, BTN)
    add(f"Stepper_{d}_Press", f"widgets/Stepper_{d}_Press.png", 16, 11, stepper(16, 11, d.lower(), PRESS), None, BTN)

NEUTRAL = "untinted: mode-neutral grey with alpha"
add("ListItem_Over", "widgets/ListItem_Over.png", 41, 24, svg(41, 24, f'<path d="{rrect(0, 0, 41, 24, 8)}" fill="rgb(128,128,128)" fill-opacity="0.16"/>'),
    (8, 8, 33, 16), NEUTRAL)
add("ListItem_Select", "widgets/ListItem_Select.png", 41, 24, svg(41, 24, f'<path d="{rrect(0, 0, 41, 24, 8)}" fill="rgb(128,128,128)" fill-opacity="0.32"/>'),
    (8, 8, 33, 16), NEUTRAL)
add("Accordion_Off", "containers/Accordion_Off.png", 41, 24, empty(41, 24), (8, 8, 33, 16), NEUTRAL)
for n, a in (("Accordion_Over", 0.12), ("Accordion_Press", 0.24), ("Accordion_Selected", 0.18)):
    add(n, f"containers/{n}.png", 41, 24, svg(41, 24, f'<path d="{rrect(0, 0, 41, 24, 8)}" fill="rgb(128,128,128)" fill-opacity="{a}"/>'),
        (8, 8, 33, 16), NEUTRAL)
add("Wearables_Divider", "windows/Wearables_Divider.png", 256, 3, hline(256, 3, 0.35), None, NEUTRAL)

WIN = "bg_*_image_overlay FloaterFocusBackgroundColor / FloaterDefaultBackgroundColor"
for n in ("Window_Background", "Window_Foreground", "Window_NoTitle_Background", "Window_NoTitle_Foreground"):
    add(n, f"windows/{n}.png", 64, 64, filled(64, 64, 20), (20, 20, 44, 44), WIN)
add("Tooltip", "widgets/Tooltip.png", 16, 16, filled(16, 16, 4), (4, 4, 12, 12), "bg_opaque_image_overlay ToolTipBgColor")
add("Inspector_Hover", "windows/Inspector_Hover.png", 48, 48, filled(48, 48, 12), (12, 12, 36, 36), "bg_opaque_image_overlay FloaterFocusBackgroundColor")
add("Inspector_Background", "windows/Inspector_Background.png", 48, 48, filled(48, 48, 12), (12, 12, 36, 36), WIN)
add("Toast_Background", "windows/Toast_Background.png", 48, 48, filled(48, 48, 12), (12, 12, 36, 36), "bg_alpha_image_overlay NotifyBoxColor (panel_toast.xml)")
add("Toast_Over", "windows/Toast_Over.png", 48, 48, filled(48, 48, 12), (12, 12, 36, 36), "bg_opaque_image_overlay NotifyBoxColor (panel_toast.xml)")
add("hint_background", "windows/hint_background.png", 48, 48, filled(48, 48, 12), (12, 12, 36, 36), "bg_opaque_image_overlay HintBg")
add("Rounded_Square", "rounded_square.png", 48, 48, filled(48, 48, 8), (8, 8, 40, 40),
    "caller colour (selection, multi-slider track, toolbar overlay BottomTrayOpaqueColor)", entry_file=True)

for n in ("ProgressBar", "ProgressBarSolid"):
    add(n, f"widgets/{n}.png", 25, 13, filled(25, 13, 6.5), (6, 6, 19, 7), "color_bar EmphasisColor (Primary)")
add("ProgressTrack", "widgets/ProgressTrack.png", 25, 13, filled(25, 13, 6.5), (6, 6, 19, 7), "color_bg ScrollbarTrackColor")
add("Badge_Background", "widgets/Badge_Background.png", 41, 25, filled(41, 25, 12), (12, 12, 29, 13), "BadgeImageColor (Error)")
add("Badge_Border", "widgets/Badge_Border.png", 41, 25, svg(41, 25, f'{ring_path(41, 25, 12, 1.5)} {grey(1)}/>'), (12, 12, 29, 13), "BadgeBorderColor (Surface)")

import xml.etree.ElementTree as ET
DEFAULT = {e.get("name"): e for e in ET.parse(sys.argv[2]).getroot().iter("texture")}
manifest = []
for t in T:
    d = DEFAULT.get(t["name"])
    if d is not None and not t["entry_file"]:
        df = d.get("file_name") or t["name"]
        assert df == t["file"], (t["name"], df, t["file"])
    t["old"] = None if d is None else {k: d.get(k) for k in ("file_name", "scale.left", "scale.bottom", "scale.right", "scale.top")}
    base = t["name"]
    svg_path = os.path.join(OUT_SVG, base + ".svg")
    with open(svg_path, "w") as f:
        f.write(t["svg"])
    png = os.path.join(SKIN, "textures", t["file"])
    os.makedirs(os.path.dirname(png), exist_ok=True)
    subprocess.run(["resvg", "-w", str(t["w"]), "-h", str(t["h"]), svg_path, png], check=True)
    manifest.append({k: t[k] for k in ("name", "file", "w", "h", "scale", "tint", "entry_file", "old")})

with open(os.path.join(WORK, "manifest.json"), "w") as f:
    json.dump(manifest, f, indent=1)
print(len(manifest), "textures")
