import os, re, sys
SKINS = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'indra', 'newview', 'skins'))
D = SKINS + '/default/xui/en/'
O = SKINS + '/idyll_material/xui/en/'

BG = {'DkGray', 'DkGray2', 'SL-Background_66', 'MouseGray', 'DkGray_66'}
TEXT = {'White': 'LabelTextColor', 'white': 'LabelTextColor', 'LtGray': 'LabelTextColor'}
CARD = 'PanelFocusBackgroundColor'
PANEL_TAGS = {'panel', 'layout_panel'}
BACK_BUTTON = {'image_unselected': ('BackButton_Off', 'PushButton_Off'),
               'image_pressed': ('BackButton_Press', 'PushButton_Press'),
               'image_hover_unselected': ('BackButton_Over', 'PushButton_Over')}
NO_ROUND_ROOT = {'panel_status_bar.xml'}
SKIP = {'floater_map.xml'}
from xml.etree import ElementTree as _ET
THEMED = {e.get('name') for e in _ET.parse(SKINS + '/idyll_material/colors.xml').getroot() if e.get('name')}

ATTR = re.compile(r'(\s)([\w.:-]+)(\s*=\s*)"([^"]*)"')
TAG = re.compile(r'<([A-Za-z_][\w.:-]*)((?:\s+[\w.:-]+\s*=\s*"[^"]*")*)(\s*/?)>', re.S)


def attrs_of(s):
    return {m.group(2): m.group(4) for m in ATTR.finditer(s)}


def set_attr(body, key, value):
    pat = re.compile(r'(\s' + re.escape(key) + r'\s*=\s*)"[^"]*"')
    if pat.search(body):
        return pat.sub(lambda m: m.group(1) + '"' + value + '"', body, count=1)
    m = re.search(r'\n([ \t]+)[\w.:-]+\s*=', body)
    indent = '\n' + m.group(1) if m else ' '
    return body + f'{indent}{key}="{value}"'


def transform(fname, text, text_only=False):
    changes = []
    first = [True]

    def tag(m):
        name, body, close = m.group(1), m.group(2), m.group(3)
        a = attrs_of(body)
        is_root = first[0]
        first[0] = False
        new = body
        for k, v in a.items():
            if 'color' not in k or k.startswith('image_color'):
                continue
            if text_only and not re.search(r'text|label|font', k) and not (name.endswith('_style') and k == 'color'):
                continue
            if k in ('bg_opaque_color', 'bg_alpha_color') and v in BG and a.get('background_visible') == 'true':
                target = 'MenuBarBgColor' if (is_root and fname in NO_ROUND_ROOT) else CARD
                new = set_attr(new, k, target); changes.append(f'<{name} {a.get("name")}> {k} {v}->{target}')
            elif (re.search(r'text|label|font', k) or (name.endswith('_style') and k == 'color')) and v in TEXT \
                    and not a.get('image_unselected', '').startswith('PushButton_Login'):
                target = 'ButtonLabelColor' if (name == 'button' and k.startswith('label')) else TEXT[v]
                new = set_attr(new, k, target); changes.append(f'<{name} {a.get("name")}> {k} {v}->{target}')
            elif k == 'text_readonly_color' and v == 'DkGray':
                new = set_attr(new, k, 'TextFgTentativeColor'); changes.append(f'<{name} {a.get("name")}> {k} {v}->TextFgTentativeColor')
        if text_only:
            return '<' + name + new + close + '>'
        if name in PANEL_TAGS and a.get('background_visible') == 'true' and not (is_root and fname in NO_ROUND_ROOT) \
                and (a.get('bg_opaque_color') in BG or a.get('bg_alpha_color') in BG) \
                and 'bg_opaque_image' not in a and 'bg_alpha_image' not in a:
            for k in ('bg_opaque_image', 'bg_alpha_image'):
                new = set_attr(new, k, 'Rounded_Square')
            for k in ('bg_opaque_image_overlay', 'bg_alpha_image_overlay'):
                new = set_attr(new, k, CARD)
            changes.append(f'<{name} {a.get("name")}> rounded card')
        elif name in PANEL_TAGS and a.get('background_visible') == 'true' and not (is_root and fname in NO_ROUND_ROOT) \
                and 'bg_opaque_image' not in a and 'bg_alpha_image' not in a \
                and all(a.get(k) is None or a.get(k) in THEMED for k in ('bg_opaque_color', 'bg_alpha_color')):
            new = set_attr(new, 'bg_opaque_image', 'Rounded_Square')
            new = set_attr(new, 'bg_alpha_image', 'Rounded_Square')
            new = set_attr(new, 'bg_opaque_image_overlay', a.get('bg_opaque_color') or 'PanelFocusBackgroundColor')
            new = set_attr(new, 'bg_alpha_image_overlay', a.get('bg_alpha_color') or 'PanelDefaultBackgroundColor')
            if a.get('border_visible') == 'true':
                new = set_attr(new, 'border_visible', 'false')
            changes.append(f'<{name} {a.get("name")}> rounded card (theme colours)')
        if a.get('image_unselected') == 'BackButton_Off':
            for k, (old, rep) in BACK_BUTTON.items():
                if a.get(k) == old:
                    new = set_attr(new, k, rep)
            new = set_attr(new, 'image_overlay', 'BackArrow_Off')
            changes.append(f'<{name} {a.get("name")}> back button pill + arrow overlay')
        if fname == 'panel_status_bar.xml' and a.get('name') == 'buyL':
            new = set_attr(new, 'image_unselected', 'PushButton_Off')
            new = set_attr(new, 'image_pressed', 'PushButton_Press')
            new = set_attr(new, 'image_hover_unselected', 'PushButton_Over')
            changes.append('<button buyL> pill images')
        return '<' + name + new + close + '>'

    # skip comments and CDATA when rewriting
    parts = re.split(r'(<!--.*?-->|<!\[CDATA\[.*?\]\]>)', text, flags=re.S)
    out = []
    for i, p in enumerate(parts):
        out.append(p if i % 2 else TAG.sub(tag, p))
    return ''.join(out), changes


if __name__ == '__main__':
    args = sys.argv[1:]
    fresh = '--fresh' in args
    log = []
    for f in (a for a in args if a != '--fresh'):
        if f in SKIP:
            continue
        src = O + f if os.path.exists(O + f) and not fresh else D + f
        text = open(src, encoding='utf-8').read()
        out, ch = transform(f, text)
        if ch:
            os.makedirs(os.path.dirname(O + f), exist_ok=True)
            open(O + f, 'w', encoding='utf-8').write(out)
            log.append(f'## {f}')
            log += ['  ' + c for c in ch]
    print('\n'.join(log))
