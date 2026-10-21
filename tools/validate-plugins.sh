#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Every built plugin through the plugin world's own validators: pluginval for each VST3, clap-validator for each
# CLAP. They test what any host may do -- state, parameters, odd block sizes, threading -- where the conformance
# suite tests bambi's own bus rules. GUI tests are skipped, so it runs headless.
#
#   tools/validate-plugins.sh <build-dir> <pluginval> <clap-validator> [strictness 1-10, default 5]
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

build=${1:?build directory}
pluginval=${2:?path to pluginval}
clapval=${3:?path to clap-validator}
level=${4:-5}

fail=0
found=0
while IFS= read -r -d '' p; do
    found=1
    echo "pluginval (strictness $level): ${p##*/}"
    "$pluginval" --strictness-level "$level" --skip-gui-tests --validate "$p" || fail=1
done < <(find "$build/plugins" -type d -name '*.vst3' -path '*_artefacts/*' -prune -print0)
while IFS= read -r -d '' p; do
    found=1
    echo "clap-validator: ${p##*/}"
    "$clapval" validate --only-failed "$p" || fail=1
done < <(find "$build/plugins" -name '*.clap' -path '*_artefacts/*' -prune -print0)

[ $found -eq 1 ] || { echo "validate: no plugins under $build/plugins" >&2; exit 2; }
[ $fail -eq 0 ] && echo "validate: every plugin passed both validators"
exit $fail
