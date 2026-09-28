#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Everything that says a change is sound, in one run: core's tests and goldens, the three plugins'
# check suites, every picture tool, and the style, layer and format checks (and the docs', where they exist).
#
#   tools/verify.sh            everything
#   tools/verify.sh --core     what needs no plugin build: seconds
#
# The picture tools are here on purpose. A constructor that called a member of a base it had not
# built yet passed every check in both effect suites and crashed the first tool that took a picture
# after audio had run -- so "it exits 0" is asserted of each of them, which costs nothing.
# conformance.sh is not run: it needs a real host and takes minutes. Run it before a release.
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
step "style"                tools/check-style.sh
step "layers"               tools/check-layers.sh
[ -x tools/check-docs.sh ] && step "docs" tools/check-docs.sh  # the private docs, where they are checked out
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
    echo "pictures"
    step "encoder"          "$(rel encoder bambi-plugin-snapshot)" "$shot"
    for mode in "" --regions --sectors --settings --copy --presets --presets-name --presets-delete; do
        step "echo $mode"   "$(rel echo bambi-echo-shot)" "$shot" $mode
    done
    for mode in "" --regions --settings --copy "--source 0"; do
        step "reverb $mode" "$(rel reverb bambi-reverb-shot)" "$shot" $mode
    done
    rm -f "$shot"
fi

if [ "$fail" -ne 0 ]; then
    echo "verify: something failed"
    exit 1
fi
echo "verify: everything passes"
