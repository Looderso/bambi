#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Remove every user preset of every bambi plugin.
#
# Until the first release the presets on this machine are test presets, and there is deliberately no
# button in a plugin that clears them. Factory presets are built into the binaries and are not
# touched. Says what it found first; asks before it deletes unless given --yes.
#
#   tools/clear-presets.sh          list, ask, delete
#   tools/clear-presets.sh --yes    list and delete
#   tools/clear-presets.sh --list   list only

set -uo pipefail

root="${BAMBI_PRESET_DIR:-$HOME/Library/Application Support/bambi/Presets}"
case "$(uname -s)" in
    Linux) root="${BAMBI_PRESET_DIR:-$HOME/.config/bambi/Presets}" ;;
esac

if [ ! -d "$root" ]; then
    echo "presets: nothing at $root"
    exit 0
fi

count=$(find "$root" -type f -name '*.json' | wc -l | tr -d ' ')
echo "presets: $count under $root"
find "$root" -type f -name '*.json' | sed "s|^$root/|  |" | sort

[ "${1:-}" = "--list" ] && exit 0
if [ "${1:-}" != "--yes" ]; then
    printf "delete all of them, and which folders are folded? [y/N] "
    read -r answer
    [ "$answer" = "y" ] || [ "$answer" = "Y" ] || { echo "presets: left as they are"; exit 0; }
fi

rm -rf "$root"
echo "presets: removed $root"
