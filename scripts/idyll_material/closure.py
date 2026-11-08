import os, sys, re
SKINS = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'indra', 'newview', 'skins'))
D = SKINS + '/default/xui/en/'
ROOTS = {
    'status bar': ['panel_status_bar.xml'],
    'chat bar / nearby chat': ['floater_fs_nearby_chat.xml', 'panel_nearby_chat_bar.xml'],
    'IM': ['floater_fs_im_session.xml', 'floater_fs_im_container.xml'],
    'notifications': ['floater_notification.xml', 'panel_notification.xml', 'floater_notifications_tabbed.xml', 'panel_toast.xml', 'panel_online_status_toast.xml', 'panel_notifications_channel.xml'],
    'People': ['floater_fs_contacts.xml', 'floater_people.xml'],
    'Inventory': ['floater_my_inventory.xml'],
    'Preferences': ['floater_preferences.xml'],
    'Build': ['floater_tools.xml'],
    'Appearance': ['floater_my_appearance.xml'],
    'World Map': ['floater_world_map.xml'],
    'minimap': ['floater_map.xml'],
    'Snapshot': ['floater_snapshot.xml'],
    'Phototools': ['floater_phototools.xml'],
    'Clouds': ['floater_id_volumetric_clouds.xml'],
}
seen_global = {}
for k, roots in ROOTS.items():
    todo = list(roots); seen = []
    while todo:
        f = todo.pop()
        if f in seen or not os.path.exists(D + f):
            continue
        seen.append(f)
        s = open(D + f, encoding='utf-8', errors='replace').read()
        for m in re.finditer(r'filename="([^"]+\.xml)"', s):
            todo.append(m.group(1))
    for f in seen:
        seen_global.setdefault(f, k)
for f in sorted(seen_global):
    print(f)
