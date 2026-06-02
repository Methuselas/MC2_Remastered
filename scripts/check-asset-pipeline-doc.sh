#!/bin/sh
# check-asset-pipeline-doc.sh — advisory drift check for docs/asset-pipeline.md.
#
# Warns (does NOT block; exit 0) when a commit changes asset loaders, the
# cook/upscale pipeline, or asset source trees WITHOUT also updating
# docs/asset-pipeline.md. Wire into .git/hooks/pre-commit to nudge maintenance.
#
# Usage:  sh scripts/check-asset-pipeline-doc.sh            (checks staged files)
#         sh scripts/check-asset-pipeline-doc.sh <ref>..<ref>   (checks a range)

DOC="docs/asset-pipeline.md"

if [ -n "$1" ]; then
    CHANGED=$(git diff --name-only "$1")
else
    CHANGED=$(git diff --cached --name-only)
fi

# Paths whose change implies the asset pipeline/inventory may have shifted.
WATCH='
tools/mc2texcook/
upscale_.*\.py
deploy_.*\.py
mc2srcdata/
release_assets/
mclib/txmmgr\.cpp
mclib/terrtxm2\.cpp
mclib/msl\.cpp
mclib/mech3d\.cpp
mclib/gvactor\.cpp
mclib/bdactor\.cpp
RenderCore/KtxLoader\.
GameOS/gameos/gos_static_prop_batcher\.cpp
GameOS/gameos/gos_terrain_indirect\.cpp
GameOS/gameos/gos_font\.cpp
GameOS/gameos/gameos_sound\.cpp
ui_editor/UiEditorImageCache\.
'

PATTERN=$(printf '%s' "$WATCH" | grep -v '^[[:space:]]*$' | paste -sd'|' -)

HITS=$(printf '%s\n' "$CHANGED" | grep -E "$PATTERN")

if [ -n "$HITS" ]; then
    if ! printf '%s\n' "$CHANGED" | grep -qx "$DOC"; then
        echo "[asset-pipeline] WARNING: asset/loader/pipeline files changed but $DOC was not updated:"
        printf '%s\n' "$HITS" | sed 's/^/    /'
        echo "[asset-pipeline] Update $DOC (inventory/owners/tiers) + bump its 'Last verified' date."
        echo "[asset-pipeline] (advisory only — commit not blocked)"
    fi
fi

exit 0
