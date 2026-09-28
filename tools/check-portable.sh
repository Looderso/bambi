#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# macOS is the only build and Windows is postponed, so core/'s portability is held by convention --
# and a convention is only real if something checks it. This runs two guards:
#
#   1. Every public header compiles standalone. libc++ pulls in headers transitively that MSVC's
#      STL does not, so a missing <algorithm> beside a std::max is invisible here.
#
#   2. core/ and tools/ build for x86_64 and the suite passes under Rosetta. Correctness only --
#      Rosetta timings prove nothing about speed, and this is still Apple clang and Apple libm.
#
#   3. The golden scenarios, rendered by both builds, null within -120 dBFS, and their control
#      traces match exactly. It is a comparison rather than a manifest because an exact hash
#      cannot be made architecture-independent at any quantisation (see tools/golden.sh). -120 dBFS
#      is chosen against the worst divergence these scenarios actually show, well below anything
#      audible, and tight enough that a real determinism bug cannot hide under it.
#
#   tools/check-portable.sh              both guards
#   tools/check-portable.sh --headers    guard 1 only (under a second)

set -uo pipefail
cd "$(dirname "$0")/.."

CXX=${CXX:-c++}
fail=0

echo "portable: compiling every public header standalone"
n=0
while IFS= read -r hdr; do
    if ! out=$("$CXX" -std=c++20 -fsyntax-only -Icore/include -x c++ "$hdr" 2>&1); then
        echo "portable: $hdr does not compile on its own --"
        echo "$out" | head -5
        fail=1
    fi
    n=$((n + 1))
done < <(find core/include -name '*.hpp' | sort)
[ "$fail" -eq 0 ] && echo "portable: $n headers, each self-contained"

if [ "${1:-}" = "--headers" ]; then
    exit "$fail"
fi

BUILD=${BUILD_X86:-build-x86}
echo "portable: configuring $BUILD for x86_64"
if ! out=$(cmake -S . -B "$BUILD" -G Ninja -DCMAKE_OSX_ARCHITECTURES=x86_64 2>&1); then
    echo "$out" | tail -20
    exit 1
fi
if ! out=$(cmake --build "$BUILD" 2>&1); then
    echo "$out" | tail -30
    exit 1
fi
echo "portable: x86_64 build clean, running the suite under Rosetta"
if ! "$BUILD/bambi-tests" | tail -3; then
    fail=1
fi

if [ ! -x build/bambi-render ] || [ ! -x build/bambi-nulltest ]; then
    echo "portable: no native build to compare against -- run: cmake --build build"
    exit 1
fi

echo "portable: nulling the golden scenarios across the two architectures"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
worst=""
while IFS='|' read -r name args column; do
    [ -z "$name" ] && continue
    # shellcheck disable=SC2086
    tool=bambi-render
    case "$name" in
      echo-*) tool=bambi-echo-render ;;
      reverb-*) tool=bambi-reverb-render ;;
    esac
    build/$tool $args "$TMP/$name.arm.wav" --trace "$TMP/$name.arm.csv" >/dev/null 2>&1
    # shellcheck disable=SC2086
    "$BUILD/$tool" $args "$TMP/$name.x86.wav" --trace "$TMP/$name.x86.csv" >/dev/null 2>&1
    out=$(build/bambi-nulltest "$TMP/$name.arm.wav" "$TMP/$name.x86.wav" --tolerance -120 2>&1)
    if [ $? -ne 0 ]; then
        echo "portable: FAIL $name -- the two architectures do not null at -120 dBFS"
        printf '%s\n' "$out" | tail -4 | sed 's/^/        /'
        fail=1
    else
        d=$(printf '%s' "$out" | sed -n 's/.*worst difference \(-*[0-9.]*\) dBFS.*/\1/p' | tail -1)
        worst="$worst${d:--999}"$'\n'
    fi
    if ! cmp -s "$TMP/$name.arm.csv" "$TMP/$name.x86.csv"; then
        echo "portable: FAIL $name -- the control traces differ between architectures"
        echo "        a trace is printed at %.6f, so this is a real divergence, not rounding"
        fail=1
    fi
done < <(grep -v '^#' tests/golden/scenarios.txt | grep .)
if [ "$fail" -eq 0 ]; then
    n=$(grep -v '^#' tests/golden/scenarios.txt | grep -c .)
    w=$(printf '%s' "$worst" | grep . | sort -g | tail -1)
    identical=$(printf '%s' "$worst" | grep -c -- '-999')
    echo "portable: $n scenarios null across the two architectures, worst $w dBFS against a -120 gate" \
         "($identical bit-identical), every control trace identical"
fi

if [ "$fail" -ne 0 ]; then
    echo "portable: the Windows build is postponed, not cancelled. Fix it here, where it costs seconds."
    exit 1
fi
