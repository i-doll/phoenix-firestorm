import os, sys, re
from xml.etree import ElementTree as ET

SKINS = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'indra', 'newview', 'skins'))
S = SKINS
D = S + '/default/xui/en/'
O = S + '/idyll_material/xui/en/'
FROZEN = re.compile(r'^(name|control_name|function|parameter|.*callback.*|label|label_selected|tool_tip|value|initial_value|'
                    r'filename|class|mouse_opaque|enabled|visible|is_toggle|enabled_control|visibility_control|'
                    r'save_rect|save_visibility|single_instance|can_close|can_minimize|can_resize|can_dock|'
                    r'can_tear_off|title|short_title|help_topic|commit_on_focus_lost|max_length|allow_text_entry|'
                    r'multi_select|draw_heading|column_padding|orientation|toolbar_id|button_display_mode|'
                    r'commands|command|use_ellipses|tab_group|layout)$')
colors = set()
for f in (S + '/default/colors.xml', S + '/idyll_material/colors.xml'):
    for e in ET.parse(f).getroot():
        if e.get('name'):
            colors.add(e.get('name'))
COLOR_ATTR = re.compile(r'(^|_)colou?r($|_)|_color$|^color$|_overlay$')

fails = []
report = []
files = []
for root, d, fs in os.walk(O):
    for f in fs:
        if f.endswith('.xml'):
            files.append(os.path.relpath(os.path.join(root, f), O))
for rel in sorted(files):
    try:
        ot = ET.parse(O + rel).getroot()
    except Exception as ex:
        fails.append(f'{rel}: parse error {ex}'); continue
    if not os.path.exists(D + rel):
        fails.append(f'{rel}: no default file'); continue
    dt = ET.parse(D + rel).getroot()
    de, oe = list(dt.iter()), list(ot.iter())
    if [e.tag for e in de] != [e.tag for e in oe]:
        fails.append(f'{rel}: element structure differs ({len(de)} vs {len(oe)})'); continue
    changed = 0
    for a, b in zip(de, oe):
        keys = set(a.attrib) | set(b.attrib)
        for k in sorted(keys):
            va, vb = a.get(k), b.get(k)
            if va == vb:
                continue
            where = f'{rel}: <{a.tag} name="{a.get("name")}"> {k}: {va!r} -> {vb!r}'
            if FROZEN.match(k):
                fails.append('FROZEN ' + where)
            else:
                changed += 1
                report.append(where)
            if vb is not None and COLOR_ATTR.search(k) and not k.startswith('image_overlay') or k in (
                    'image_overlay_color', 'image_overlay_selected_color', 'image_overlay_disabled_color'):
                if vb and re.match(r'^[A-Za-z][\w.-]*$', vb) and vb not in colors:
                    fails.append(f'UNKNOWN COLOUR {where}')
    report.append(f'== {rel}: {changed} attribute changes')
print('\n'.join(report))
print(f'{len(files)} overrides checked')
if fails:
    print('FAIL'); print('\n'.join(fails)); sys.exit(1)
print('PASS')
