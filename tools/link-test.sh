#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Cross-process link bus test. The unit tests run several LinkBus objects inside one
# process, which shares an address space, a compiler and a clock with itself — none of which
# a plugin gets when a host sandboxes it across processes. This runs real ones, and exercises
# the lifecycle cases only real processes have: being killed, being suspended,
# and dying in the middle of an edit.

set -uo pipefail
cd "$(dirname "$0")/.."
LINK=build/bambi-link
[ -x "$LINK" ] || { echo "build/bambi-link not found — run: cmake --build build" >&2; exit 2; }

uuid() { python3 -c "import uuid;print(uuid.uuid4())"; }
SESSION=$(uuid); OTHER=$(uuid); RESHAPE=$(uuid); IDLE=$(uuid); SLEEP=$(uuid); EDIT=$(uuid)
TMP=$(mktemp -d)
cleanup() {
  # -9 so that a process left stopped by a failed test does not survive the run.
  kill -9 $(jobs -p) 2>/dev/null
  for s in "$SESSION" "$OTHER" "$RESHAPE" "$IDLE" "$SLEEP" "$EDIT"; do
    $LINK unlink --session "$s" >/dev/null 2>&1
  done
  rm -rf "$TMP"
}
trap cleanup EXIT

fail=0
step() { printf '  %-58s' "$1"; }
ok()   { echo "ok"; }
bad()  { echo "FAIL — $1"; fail=1; }
flat() { tr '\n' ' ' < "$1"; }

# ---- 1. two publishers in separate processes are both visible to a third ----------------
step "two publishers seen by a separate watcher process"
$LINK publish --session "$SESSION" --label kick --seconds 3 > "$TMP/p1" 2>&1 &
$LINK publish --session "$SESSION" --label pad  --seconds 3 > "$TMP/p2" 2>&1 &
sleep 0.4
if $LINK watch --session "$SESSION" --seconds 1.5 --expect 2 > "$TMP/w" 2>&1; then ok
else bad "$(flat "$TMP/w")"; fi

# ---- 2. a command crosses a process boundary and lands on the right instance ------------
step "command delivered across processes"
$LINK publish --session "$SESSION" --label target --seconds 2.5 > "$TMP/t" 2>&1 &
sleep 0.4
TARGET=$(head -1 "$TMP/t")
if [ -z "$TARGET" ]; then
  bad "target never announced its uuid"
else
  $LINK send --session "$SESSION" --target "$TARGET" --param render.width --value 55 >/dev/null 2>&1
  sleep 0.6
  if grep -q "command render.width = 55.000" "$TMP/t"; then ok
  else bad "target never reported the command: $(flat "$TMP/t")"; fi
fi
wait 2>/dev/null

# ---- 3. sessions are isolated across processes ------------------------------------------
step "a different session sees nothing"
$LINK publish --session "$SESSION" --label lonely --seconds 2 > "$TMP/p3" 2>&1 &
sleep 0.4
if $LINK watch --session "$OTHER" --seconds 0.8 --expect 0 > "$TMP/w2" 2>&1; then ok
else bad "$(flat "$TMP/w2")"; fi
wait 2>/dev/null

# ---- 4. a killed process is reaped, not left haunting the scene -------------------------
step "a killed instance is reaped by heartbeat timeout"
$LINK publish --session "$SESSION" --label doomed --seconds 30 > "$TMP/p4" 2>&1 &
DOOMED=$!
sleep 0.5
kill -9 $DOOMED 2>/dev/null
wait $DOOMED 2>/dev/null
# The slot cannot be released by a process that was killed, so only the heartbeat can clear
# it. That is exactly the case the timeout exists for.
sleep 2.5
if $LINK watch --session "$SESSION" --seconds 0.5 --expect 0 > "$TMP/w3" 2>&1; then ok
else bad "$(flat "$TMP/w3")"; fi

# ---- 5. a path edit — the static section — reaches another process ----------------------
step "a path edit reaches another process"
$LINK publish --session "$RESHAPE" --label shifter --seconds 3 --reshape-after 1.0 > "$TMP/r" 2>&1 &
sleep 0.4
if $LINK watch --session "$RESHAPE" --seconds 1.5 --expect 1 --expect-reshape > "$TMP/wr" 2>&1; then
  if grep -qE "points [1-9][0-9]*" "$TMP/wr"; then ok   # a path is sent with as many points as its length needs
  else bad "the path itself never arrived: $(flat "$TMP/wr")"; fi
else
  bad "$(flat "$TMP/wr")"
fi
wait 2>/dev/null

# ---- 6. liveness belongs to the plugin, not to its audio thread -------------------------
step "an instance stays visible after its audio stops"
$LINK publish --session "$IDLE" --label idle --seconds 5 --audio-stops-after 0.3 > "$TMP/i" 2>&1 &
# Well past the 2 s heartbeat timeout, measured from the moment audio stopped. Had the
# heartbeat ridden on the audio thread, the instance would already be gone — which is what a
# host stopping its transport would do to every source in the scene.
sleep 3.0
if $LINK watch --session "$IDLE" --seconds 0.6 --expect 1 > "$TMP/wi" 2>&1 \
   && grep -q "audio stopped" "$TMP/i"; then ok
else bad "$(flat "$TMP/wi") | publisher: $(flat "$TMP/i")"; fi
wait 2>/dev/null

# ---- 7. a suspended instance comes back, without evicting whoever took its slot ---------
step "a suspended instance rejoins without evicting its successor"
$LINK publish --session "$SLEEP" --label sleeper --seconds 10 > "$TMP/s1" 2>&1 &
SLEEPER=$!
sleep 0.5
# A closed laptop lid or a debugger breakpoint does exactly this.
kill -STOP $SLEEPER
sleep 2.6
# Opening reaps the stale sleeper and claims its slot.
$LINK publish --session "$SLEEP" --label newcomer --seconds 6 > "$TMP/s2" 2>&1 &
NEWCOMER=$!
sleep 0.5
kill -CONT $SLEEPER
sleep 0.8
# Four conditions, each necessary: both visible, both with their paths, the sleeper noticed
# and rejoined -- and the newcomer did not have to. A newcomer evicted by the sleeper leaving
# its old slot would rejoin within one tick and look perfectly healthy by the time anyone
# watched; only its own log shows that it was knocked out.
if $LINK watch --session "$SLEEP" --seconds 0.6 --expect 2 > "$TMP/ws" 2>&1 \
   && grep -q "rejoined" "$TMP/s1" \
   && ! grep -q "rejoined" "$TMP/s2" \
   && [ "$(grep -cE 'points [1-9][0-9]*' "$TMP/ws")" = "2" ]; then ok
else bad "$(flat "$TMP/ws") | sleeper: $(flat "$TMP/s1") | newcomer: $(flat "$TMP/s2")"; fi
kill $SLEEPER $NEWCOMER 2>/dev/null
wait 2>/dev/null

# ---- 8. a sender that dies mid-edit cannot leave the target's host mid-gesture ----------
step "a sender killed mid-edit leaves no gesture open"
$LINK publish --session "$EDIT" --label target --seconds 6 > "$TMP/e" 2>&1 &
sleep 0.4
EDIT_TARGET=$(head -1 "$TMP/e")
$LINK send --session "$EDIT" --target "$EDIT_TARGET" --param render.width --value 40 \
     --gesture begin --hold 30 > /dev/null 2>&1 &
SENDER=$!
sleep 0.6
# kill -9: no end is ever sent and the slot is never released -- only the heartbeat can tell
# the target this sender is gone. This is the case no inbox policy can cover.
kill -9 $SENDER 2>/dev/null
wait $SENDER 2>/dev/null
sleep 2.8
if grep -q "edit begin render.width" "$TMP/e" \
   && grep -q "edit value render.width = 40.000" "$TMP/e" \
   && grep -q "edit end render.width (sender gone)" "$TMP/e"; then ok
else bad "target: $(flat "$TMP/e")"; fi
wait 2>/dev/null

[ "$fail" = "0" ] && echo "link: all cross-process checks passed"
exit $fail
