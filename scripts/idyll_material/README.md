# idyll_material skin scripts

These scripts generate the Idyll skin's textures and XUI overrides from the `default` skin. Run them from the repo root with a Python that has Pillow. Texture generation also needs `resvg` and ImageMagick (`magick`).

## Textures

- `build.sh [work_dir]` runs the four steps below. It rewrites the PNGs, `textures.xml` and the skin preview. The SVGs, manifest and contact sheet go to `work_dir`, or to a new temp dir.
- `gen.py` draws every control texture as SVG, renders it into `idyll_material/textures/` and writes `manifest.json`.
- `mkxml.py` writes `idyll_material/textures/textures.xml` from the manifest, including the tint-roles block.
- `sheet.py` draws a contact sheet of the textures in dark and light mode, for checking by eye.
- `preview.py` draws the skin preview image shown in Preferences.
- `palette.json` holds the reference light and dark palettes that `sheet.py` and `preview.py` use.

## Icon tint roles

- `roles.json` says, for each icon image, whether it is drawn as a glyph (tinted with the theme colour) or as art (drawn untinted).
- `emit_roles.py` rewrites the tint-roles block in `textures.xml` from `roles.json`. It also writes brightened copies of glyph PNGs that are too dark to tint well.

## XUI overrides

- `overrides.txt` lists every file in `idyll_material/xui/en/` and how it was made:
  - `restyle`: made by `restyle.py`;
  - `list_accents`: made by `list_accents.py`;
  - `hand`: edited by hand.
- `restyle.py FILE...` changes colours, background images and back buttons in the named XUI files (paths relative to `xui/en/`). With `--fresh` it starts from the default file, not the existing override.
- `list_accents.py` rewrites the 18 list-row files with accent hover and selection, and maps their white text the way `restyle.py` does.
- `closure.py` prints every XUI file that the everyday floaters load.
- `scan.py FILE...` lists the colours and images in those files that the skin doesn't theme yet.
- `check_overrides.py` compares each override with its default file. It fails if the element structure differs, if an attribute it should not touch (names, callbacks, labels, controls and the like) changed, or if a colour name doesn't exist.

## After merging upstream XUI changes

Run the checker:

```sh
python3 scripts/idyll_material/check_overrides.py
```

If it fails, look up each listed file in `overrides.txt` and regenerate it:

```sh
python3 scripts/idyll_material/restyle.py --fresh panel_status_bar.xml   # restyle files
python3 scripts/idyll_material/list_accents.py                           # list_accents files
```

For `hand` files, diff the new default file against the override and port the changes yourself.

The checker only catches structural changes. An upstream change to a size or position in a copied file still passes, and the override hides it. Regenerating every `restyle` and `list_accents` file after each merge avoids that. When the default file hasn't changed, the output is identical.

```sh
python3 scripts/idyll_material/restyle.py --fresh $(awk '$1=="restyle"{print $2}' scripts/idyll_material/overrides.txt)
python3 scripts/idyll_material/list_accents.py
python3 scripts/idyll_material/check_overrides.py
```

To see which `hand` files an upstream merge touched:

```sh
git diff --name-only OLD..NEW -- indra/newview/skins/default/xui/en \
  | sed 's|.*/xui/en/||' | grep -Fxf <(awk '$1=="hand"{print $2}' scripts/idyll_material/overrides.txt)
```
