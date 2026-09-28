#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Save a file, see the tests run. Polls mtimes so it needs nothing installed.
#   tools/watch.sh [ctest args...]
set -uo pipefail
cd "$(dirname "$0")/.."

[ -d build ] || cmake -B build -G Ninja >/dev/null

fingerprint() {
  find core tools CMakeLists.txt -type f \
       \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name 'CMakeLists.txt' \) \
       -not -name 'doctest.h' -exec stat -f '%m %N' {} + 2>/dev/null | sort
}

last=""
printf 'watching core/ — ctrl-c to stop\n\n'
while true; do
  now="$(fingerprint)"
  if [ "$now" != "$last" ]; then
    last="$now"
    printf '\033[2J\033[H'
    date '+%H:%M:%S'
    if cmake --build build 2>&1 | grep -v '^\[' ; then :; fi
    if cmake --build build >/dev/null 2>&1; then
      ./build/bambi-tests "$@" | tail -6
    else
      printf '\033[31mbuild failed\033[0m\n'
      cmake --build build 2>&1 | grep -E 'error|Error' | head -20
    fi
  fi
  sleep 0.3
done
