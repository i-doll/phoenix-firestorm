import os, sys, json, re
from PIL import Image

H = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, H)
from mkxml import roles_block

R = os.path.normpath(os.path.join(H, '..', '..', 'indra', 'newview', 'skins'))
roles = json.load(open(H + '/roles.json'))

normalised = []
for n, s in sorted(roles.items()):
    if s['role'] != 'glyph' or s['p95'] >= 0.7 or 'Disabled' in n or 'Background' in n:
        continue
    if not s['file'].endswith('.png'):
        continue
    src = R + '/default/textures/' + s['file']
    dst = R + '/idyll_material/textures/' + s['file']
    im = Image.open(src).convert('RGBA')
    k = 1.0 / max(s['p95'], 0.05)
    px = [(min(255, round(r * k)), min(255, round(g * k)), min(255, round(b * k)), a) for r, g, b, a in im.getdata()]
    out = Image.new('RGBA', im.size)
    out.putdata(px)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    out.save(dst, optimize=True)
    normalised.append((n, s['file'], round(s['p95'], 2)))

tx = R + '/idyll_material/textures/textures.xml'
text = open(tx).read()
text = re.sub(r'\n  <!-- tint roles -->.*?(?=</textures>)', '\n', text, flags=re.S)
text = text.replace('</textures>', '\n'.join(roles_block()) + '\n</textures>')
text = text.replace('\n\n  <!-- tint roles -->', '\n  <!-- tint roles -->')
open(tx, 'w').write(text)
print(len(roles), 'role entries;', len(normalised), 'normalised')
for x in normalised:
    print(*x)
