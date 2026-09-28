#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# core/ is layered, and the layering is only real if something checks it.
#
# Fails if a header or source in core/ includes from a higher layer than its own. Same layer is
# allowed -- a group's files may use each other freely. Tests are exempt on purpose: a test may
# reach anywhere, because it is not part of what the libraries link.
#
#   tools/check-layers.sh

set -uo pipefail
cd "$(dirname "$0")/.."

#  Lowest first. A group may include its own layer and anything below it.
#  Derived from the real include graph, not from taste.
LAYERS=(
    "math io"          # 0  vectors, spherical harmonics, WAV: depend on nothing
    "dsp path region"  # 1  the FFT and features; trajectories and their shape; regions
    "patch"            # 2  parameters, state, undo, identity, json -- a patch contains a trajectory
    "mod"              # 3  the modulation matrix, over the patch and the path
    "link scene encode echo reverb"  # 4  the bus, the scene's geometry, an engine
)

layer_of() {
    local g=$1 i=0
    for row in "${LAYERS[@]}"; do
        for name in $row; do [ "$name" = "$g" ] && { echo "$i"; return 0; }; done
        i=$((i + 1))
    done
    echo "-1"
}

fail=0
while IFS= read -r file; do
    group=$(basename "$(dirname "$file")")
    mine=$(layer_of "$group")
    if [ "$mine" = "-1" ]; then
        echo "layers: $file is in '$group', which no layer declares -- add it to LAYERS in $0"
        fail=1
        continue
    fi
    while IFS= read -r inc; do
        theirs=$(layer_of "$inc")
        if [ "$theirs" = "-1" ]; then
            echo "layers: $file includes bambi/$inc/, which no layer declares"
            fail=1
        elif [ "$theirs" -gt "$mine" ]; then
            echo "layers: $file ($group, layer $mine) includes bambi/$inc/ (layer $theirs) -- upward"
            fail=1
        fi
    done < <(grep -oE 'bambi/[a-z]+/[a-z0-9]+\.hpp' "$file" | cut -d/ -f2 | sort -u)
done < <(find core/include core/src -name '*.hpp' -o -name '*.cpp' | sort)

#  Above core: `ui` and `host` do not know each other, and `editor` is where they meet.
#  The build graph stops a .cpp; it does not stop a header only a plugin compiles.
above() {
    local dir=$1; shift
    local pattern=$1; shift
    local hits
    hits=$(grep -rnE "#include \"bambi/($pattern)/" "$dir" --include='*.h' --include='*.hpp' --include='*.cpp' || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "layers: $dir must not include bambi/($pattern)/"
        fail=1
    fi
}
above core   'ui|host|editor'
above ui     'host|editor'
above host   'ui|editor'

if [ "$fail" -ne 0 ]; then
    echo "layers: core's layering is what keeps it from going flat again. Move the type, do not add the include."
    exit 1
fi
echo "layers: ui and host do not include each other, and every include in core/ runs downward or sideways ($(find core/include core/src -name '*.hpp' -o -name '*.cpp' | wc -l | tr -d ' ') files)"
