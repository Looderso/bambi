#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Nests the helper app inside a plugin bundle's Contents/Helpers and signs the bundle again (ad hoc), since a
# bundle changed after signing would not launch. Run after the build, on the built bundle and on the installed one.
#
#   nest.sh <helper.app> <plugin bundle> [<plugin bundle> ...]
set -euo pipefail
helper=$1; shift
for bundle in "$@"; do
    [ -d "$bundle/Contents" ] || continue
    rm -rf "$bundle/Contents/Helpers"
    mkdir -p "$bundle/Contents/Helpers"
    cp -R "$helper" "$bundle/Contents/Helpers/"
    codesign --force --deep -s - "$bundle" 2>/dev/null || true
done
