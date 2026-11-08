#!/bin/sh
# Usage: build.sh [work_dir]   (work_dir keeps the SVGs, manifest and contact sheet; default: a new temp dir)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
R=$HERE/../../indra/newview/skins
WORK=${1:-$(mktemp -d)}
mkdir -p "$WORK"
"${PYTHON:-python3}" -I "$HERE/gen.py" "$R/idyll_material" "$R/default/textures/textures.xml" "$WORK"
"${PYTHON:-python3}" -I "$HERE/mkxml.py" "$WORK/manifest.json" "$R/idyll_material/textures/textures.xml"
"${PYTHON:-python3}" -I "$HERE/sheet.py" "$R/idyll_material" "$WORK/manifest.json" "$WORK/contact.png"
"${PYTHON:-python3}" -I "$HERE/preview.py" "$R/default/textures/skinspreview/idyll_material.jpg" "$WORK"
echo "work files in $WORK"
