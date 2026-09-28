#!/usr/bin/env bash
#  The suite's one property: every plugin negotiates its ambisonic layout and accepts a sidechain, in both
#  formats, in a real host. bambi-conformance asks it of the sources and of the installed VST3s and CLAPs.
#
#  Needs the plugin build, which also installs the plugins: cmake -B build-plugin -G Ninja -DBAMBI_BUILD_PLUGIN=ON
#    tools/conformance.sh [build-dir]
set -uo pipefail
cd "$(dirname "$0")/.."
CONF="${1:-build-plugin}/tests/bambi-conformance_artefacts/Release/bambi-conformance"
[ -x "$CONF" ] || { echo "conformance: bambi-conformance is not built ($CONF)"; exit 1; }
exec "$CONF"
