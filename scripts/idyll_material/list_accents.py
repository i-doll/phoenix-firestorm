import os, re
from restyle import transform
SKINS = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'indra', 'newview', 'skins'))
D = SKINS + '/default/xui/en/'
O = SKINS + '/idyll_material/xui/en/'
PANELS = ['panel_teleport_history_item.xml', 'panel_body_parts_list_item.xml', 'panel_blocked_list_item.xml',
          'panel_pick_list_item.xml', 'panel_dummy_clothing_list_item.xml', 'panel_avatar_list_item.xml',
          'panel_group_list_item.xml', 'panel_deletable_wearable_list_item.xml', 'panel_classifieds_list_item.xml',
          'panel_inventory_item.xml', 'panel_conversation_log_list_item.xml', 'panel_group_list_item_short.xml',
          'panel_camera_preset_item.xml', 'panel_clothing_list_item.xml', 'widgets/panel_camera_item.xml']
COL = {'ListItem_Over': 'ScrollHoveredColor', 'ListItem_Select': 'ScrollSelectedBGColor'}


def panel(text):
    def rep(m):
        ind, name = m.group(1), m.group(2)
        return f'{ind}image_name="Rounded_Square"\n{ind.lstrip(chr(10))}color="{COL[name]}"'
    out, n = re.subn(r'(\n[ \t]*)image_name="(ListItem_Over|ListItem_Select)"', rep, text)
    return out, n


def widget(text):
    n = 0
    for attr, name in (('hover_image', 'ListItem_Over'), ('selected_image', 'ListItem_Select')):
        pat = re.compile(r'(\n([ \t]*)' + attr + r')="' + name + '"')
        text, k = pat.subn(lambda m: f'{m.group(1)}="Rounded_Square"\n{m.group(2)}{attr}_color="{COL[name]}"', text)
        n += k
    return text, n


if __name__ == '__main__':
    for f in PANELS:
        src = open(D + f, encoding='utf-8').read()
        out, n = panel(src)
        assert n == 2, (f, n)
        out = transform(f, out, text_only=True)[0]
        os.makedirs(os.path.dirname(O + f), exist_ok=True)
        open(O + f, 'w', encoding='utf-8').write(out)
        print(f, n)
    for f in ['widgets/inventory_list_item.xml', 'widgets/wearable_outfit_list_item.xml', 'widgets/emoji_complete.xml']:
        src = open(D + f, encoding='utf-8').read()
        out, n = widget(src)
        assert n >= 1, (f, n)
        out = transform(f, out, text_only=True)[0]
        open(O + f, 'w', encoding='utf-8').write(out)
        print(f, n)
