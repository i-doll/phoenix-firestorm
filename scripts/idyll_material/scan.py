import os, re, sys, json, collections
from xml.etree import ElementTree as ET
SKINS = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'indra', 'newview', 'skins'))
S = SKINS
D = S + '/default/xui/en/'
O = S + '/idyll_material/xui/en/'
themed = {e.get('name') for e in ET.parse(S + '/idyll_material/colors.xml').getroot() if e.get('name')}
default_colors = {e.get('name') for e in ET.parse(S + '/default/colors.xml').getroot() if e.get('name')}
idyll_tex = {e.get('name') for e in ET.parse(S + '/idyll_material/textures/textures.xml').getroot() if e.get('name')}
decl = {e.get('name'): (e.get('file_name') or e.get('name')) for e in ET.parse(S + '/default/textures/textures.xml').getroot() if e.get('name')}
T = S + '/idyll_material/textures/'
idyll_files = {os.path.relpath(os.path.join(r, f), T) for r, d, fs in os.walk(T) for f in fs}
replaced = idyll_tex | {n for n, f in decl.items() if f in idyll_files}
COLOR = re.compile(r'colou?r')
IMAGE = re.compile(r'image|^icon$|bg_.*image')
files = sys.argv[1:]
for f in files:
    p = (O + f) if os.path.exists(O + f) else (D + f)
    t = ET.parse(p)
    hits = collections.Counter()
    for el in t.iter():
        for k, v in el.attrib.items():
            if COLOR.search(k) and not k.startswith('glyph'):
                if v in default_colors and v not in themed:
                    hits[f'colour {k}={v}'] += 1
                elif re.match(r'^[\d. ]+$', v):
                    hits[f'literal {k}={v}'] += 1
            elif IMAGE.search(k) and k not in ('image_overlay_alignment', 'scale_image', 'image_top_pad', 'image_bottom_pad', 'imgoverlay_label_space'):
                if v in replaced or v in ('', 'transparent.j2c', 'Blank'):
                    continue
                if v in idyll_tex:
                    continue
                if k == 'image_overlay':
                    continue
                hits[f'image {k}={v}'] += 1
            elif k in ('bevel_style', 'border_visible', 'border'):
                if v not in ('none', 'false'):
                    hits[f'{k}={v}'] += 1
    print(f'## {f} ({"override" if p.startswith(O) else "default"})')
    for k, v in sorted(hits.items()):
        print(f'   {v:3d} {k}')
