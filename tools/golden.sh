#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Golden-file regression for bambi-render.
#
# Renders the scenarios in tests/golden/scenarios.txt and compares hashes of both the audio and
# the control trace against tests/golden/manifest.txt. The trace matters as much as the audio: a
# modulation bug can leave audio that still sounds plausible, and hashing only the wav would let
# it through.
#
#   tools/golden.sh            check
#   tools/golden.sh --update   re-record the manifest (review the diff before committing)
#
# Inputs are generated pink noise with a fixed seed, so no audio fixtures live in the repo.
#
# This is a per-toolchain manifest, deliberately. An exact hash cannot be made architecture-
# independent at any quantisation: a one-ULP difference near 0.1 full scale is ~7.5e-9, a 24-bit
# LSB is 1.2e-7, so a meaningful share of samples sit close enough to a rounding boundary to cross
# it, and a scenario has millions of samples. Every coarser word length has the same problem. So
# the audio hashes are Apple-clang / arm64, and the architecture-independent check is a different
# instrument: tools/check-portable.sh renders these same scenarios with both builds and nulls them
# at -120 dBFS. The control traces are already toolchain-independent, because they are printed at
# %.6f and are therefore tolerance goldens.

set -uo pipefail
cd "$(dirname "$0")/.."

RENDER=build/bambi-render
MANIFEST=tests/golden/manifest.txt
UPDATE=0
[ "${1:-}" = "--update" ] && UPDATE=1

if [ ! -x "$RENDER" ]; then
  echo "build/bambi-render not found — run: cmake --build build" >&2
  exit 2
fi

hash_of() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
  else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

SCENARIOS=$(grep -v '^#' tests/golden/scenarios.txt | grep .)

fail=0
out=""
while IFS='|' read -r name args column; do
  [ -z "$name" ] && continue
  #  A scenario named echo-* is Echo's and reverb-* is Reverb's; each is rendered by its own tool.
  tool=$RENDER
  case "$name" in
    echo-*) tool=build/bambi-echo-render ;;
    reverb-*) tool=build/bambi-reverb-render ;;
  esac
  # shellcheck disable=SC2086
  if ! $tool $args "$TMP/$name.wav" --trace "$TMP/$name.csv" >/dev/null 2>"$TMP/err"; then
    echo "FAIL  $name — render exited nonzero:"; sed 's/^/        /' "$TMP/err"; fail=1; continue
  fi
  if [ -n "$column" ]; then
    if ! awk -F, -v col="$column" '
        NR==1 { for (i=1;i<=NF;i++) if ($i==col) k=i; if (!k) { print "nocol"; exit 1 } next }
        { if (NR==2) { lo=$k; hi=$k } else { if ($k<lo) lo=$k; if ($k>hi) hi=$k } }
        END { exit (hi-lo > 1e-6) ? 0 : 1 }' "$TMP/$name.csv"; then
      echo "FAIL  $name — '$column' never moves; this scenario tests nothing"; fail=1
    fi
  fi
  line="$name $(hash_of "$TMP/$name.wav") $(hash_of "$TMP/$name.csv")"
  out="$out$line"$'\n'
done <<< "$SCENARIOS"

if [ "$UPDATE" = "1" ]; then
  printf '%s' "$out" > "$MANIFEST"
  echo "recorded $(wc -l < "$MANIFEST" | tr -d ' ') scenarios to $MANIFEST"
  exit 0
fi

if [ ! -f "$MANIFEST" ]; then
  echo "no manifest at $MANIFEST — run: tools/golden.sh --update" >&2
  exit 2
fi

# Compare line by line so the report names what moved rather than dumping a diff.
while read -r name wav csv; do
  [ -z "$name" ] && continue
  got=$(printf '%s' "$out" | awk -v n="$name" '$1==n {print $2" "$3}')
  if [ -z "$got" ]; then echo "FAIL  $name — scenario missing from this run"; fail=1; continue; fi
  gw=${got% *}; gc=${got#* }
  if [ "$gw" != "$wav" ] && [ "$gc" != "$csv" ]; then
    echo "FAIL  $name — audio AND control trace both changed"; fail=1
  elif [ "$gw" != "$wav" ]; then
    echo "FAIL  $name — audio changed, control trace did not (encoding or SH path)"; fail=1
  elif [ "$gc" != "$csv" ]; then
    echo "FAIL  $name — control trace changed, audio did not (features or modulation)"; fail=1
  fi
done < "$MANIFEST"

# Not a golden file but the same class of guarantee: a render must not depend on the host's
# buffer size, or an offline bounce differs from realtime.
BASE=""
for b in 1 64 128 333 1024; do
  $RENDER --noise 2 --order 3 --speed 70 --width 30 --generator lissajous \
          --mod level:motion.speed=1 --mod attack:render.width=0.5 \
          --region spot --param region1.softness=40 --param region1.yaw_rate=30 --mod region1:motion.speed=0.5 \
          --block "$b" "$TMP/b$b.wav" >/dev/null 2>&1
  h=$(hash_of "$TMP/b$b.wav")
  [ -z "$BASE" ] && BASE="$h"
  if [ "$h" != "$BASE" ]; then echo "FAIL  block size $b changes the render"; fail=1; fi
done

# The same for Echo, where it is harder won: a loop reads what it wrote, and two of these periods are
# shorter than any block a host uses. Everything a frame can move, moves.
BASE=""
for b in 1 64 128 333 1024; do
  build/bambi-echo-render --order 3 --seconds 2 --voices 5 \
          --set tap1.synced=0 --set tap1.ms=20 --set tap1.feedback=-1 \
          --set tap2.on=1 --set tap2.az=30 --set tap2.el=25 --set tap2.skew=0.4 --set tap2.swing=0.3 \
          --set tap3.on=1 --set tap3.blur=25 --set tap3.low_cut=200 --set send.kind=spot \
          --at 0.5 bpm=133 --at 0.7 send.amount=0.3 --at 0.9 tap2.on=0 --at 1.2 tap2.on=1 --at 1.4 send.side=outside --at 1.6 reset \
          --block "$b" "$TMP/e$b.wav" >/dev/null 2>&1
  h=$(hash_of "$TMP/e$b.wav")
  [ -z "$BASE" ] && BASE="$h"
  if [ "$h" != "$BASE" ]; then echo "FAIL  block size $b changes Echo's render"; fail=1; fi
done

# And for Reverb, where a hop's crossfades, the tail's drift and twelve delay lines all have to agree
# on an absolute position rather than on where a block happened to start. Everything a hop can move,
# moves: the room, the regions, the amounts, the line count, and a reset in the middle.
BASE=""
for b in 1 64 128 333 1024; do
  build/bambi-reverb-render --order 3 --seconds 2 --voices 5 \
          --set send.kind=spot --set send.size=60 --set return.kind=band \
          --at 0.4 send.yaw=140 --at 0.6 return.amount=0.4 --at 0.9 room.size=32 --at 1.2 lines=8 \
          --at 1.4 send.side=outside --at 1.6 reset \
          --block "$b" "$TMP/r$b.wav" >/dev/null 2>&1
  h=$(hash_of "$TMP/r$b.wav")
  [ -z "$BASE" ] && BASE="$h"
  if [ "$h" != "$BASE" ]; then echo "FAIL  block size $b changes Reverb's render"; fail=1; fi
done

if [ "$fail" = "0" ]; then
  echo "golden: $(grep -c . "$MANIFEST") scenarios match, and every render is block-size independent"
fi
exit $fail
