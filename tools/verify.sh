#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Everything that says a change is sound, in one run: core's tests and goldens, every plugin's check
# suite, every picture tool, and the style, layer, format and docs checks.
#
#   tools/verify.sh            everything
#   tools/verify.sh --core     what needs no plugin build: seconds
#
# The picture tools are here on purpose. A constructor that called a member of a base it had not
# built yet passed every check in both effect suites and crashed the first tool that took a picture
# after audio had run -- so "it exits 0" is asserted of each of them, which costs nothing.
# conformance.sh is not run: it loads the installed bundles as a host does and takes a minute. Run it before a release.
set -uo pipefail
cd "$(dirname "$0")/.."

fail=0
step() {   # step "name" command...
    local name=$1; shift
    local out
    if out=$("$@" 2>&1); then
        printf '  ok    %s\n' "$name"
    else
        printf '  FAIL  %s\n' "$name"
        printf '%s\n' "$out" | tail -15 | sed 's/^/        /'
        fail=1
    fi
}

echo "core"
step "build"                cmake --build build
step "tests"                ./build/bambi-tests
step "goldens"              tools/golden.sh
if command -v node >/dev/null 2>&1; then
    step "device scripts"   bash -c 'node tools/live-stub/max/test-match.js && node tools/live-stub/max/test-launch.js && node tools/live-stub/max/test-routing.js'
fi
step "style"                tools/check-style.sh
step "layers"               tools/check-layers.sh
step "docs" tools/check-docs.sh
step "format"               tools/format.sh --check
step "notices"              tools/release/notices.py --check

if [ "${1:-}" != "--core" ]; then
    rel() { echo "build-plugin/plugins/$1/$2_artefacts/Release/$2"; }
    shot=$(mktemp -t bambi-verify).png
    echo "plugins"
    step "build"            cmake --build build-plugin
    step "encoder checks"   "$(rel encoder bambi-plugin-check)"
    step "echo checks"      "$(rel echo bambi-echo-check)"
    step "reverb checks"    "$(rel reverb bambi-reverb-check)"
    step "decoder checks"   "$(rel decoder bambi-decoder-check)"
    echo "pictures"
    step "encoder"          "$(rel encoder bambi-plugin-snapshot)" "$shot"
    for mode in "" --regions --sectors --settings --copy --presets --presets-name --presets-delete --axis --every-pass; do
        step "echo $mode"   "$(rel echo bambi-echo-shot)" "$shot" $mode
    done
    for mode in "" --regions --settings --copy --room "--source 0"; do
        step "reverb $mode" "$(rel reverb bambi-reverb-shot)" "$shot" $mode
    done
    for mode in "" --speakers --sources; do
        step "decoder $mode" "$(rel decoder bambi-decoder-shot)" "$shot" $mode
    done
    # the live host's window, when the tree is configured with -DBAMBI_BUILD_HOST=ON
    if grep -q '^BAMBI_BUILD_HOST:BOOL=ON' build-plugin/CMakeCache.txt 2>/dev/null; then
        host_shot="build-plugin/apps/live-host/bambi-live-host-shot_artefacts/Release/bambi-live-host-shot"
        for mode in --routing --wires --decoders --timeline --session --interface --outputs --inputs --midi; do
            step "live host $mode" "$host_shot" "$shot" $mode
        done
    fi
    rm -f "$shot"
fi

if [ "$fail" -ne 0 ]; then
    echo "verify: something failed"
    exit 1
fi
echo "verify: everything passes"
