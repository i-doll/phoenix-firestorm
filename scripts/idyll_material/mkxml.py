import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))


def roles_block():
    roles = json.load(open(os.path.join(HERE, 'roles.json')))
    lines = ['  <!-- tint roles -->']
    for n in sorted(roles, key=str.lower):
        lines.append(f'  <texture name="{n}" tint_role="{roles[n]["role"]}" />')
    return lines


if __name__ == '__main__':
    m = json.load(open(sys.argv[1]))
    lines = ['<?xml version="1.0" encoding="utf-8" standalone="yes" ?>', '<textures version="101">']
    for t in m:
        if t['scale'] is None and not t['entry_file']:
            continue
        attrs = f'name="{t["name"]}" file_name="{t["file"]}"'
        if t['entry_file'] or t['old'] is None:
            attrs += ' preload="true"'
        if t['scale']:
            l, b, r, tp = t['scale']
            attrs += f' scale.left="{l}" scale.top="{tp}" scale.right="{r}" scale.bottom="{b}"'
        lines.append(f'  <texture {attrs} />')
    lines += roles_block()
    lines.append('</textures>')
    open(sys.argv[2], 'w').write('\n'.join(lines) + '\n')
