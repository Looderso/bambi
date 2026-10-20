# 0064 · Every effect has two levels, wet and dry — not a mix

**Status:** accepted

**Date:** 2026-09-19  **Status when written:** decided by the owner  **Scope:** suite · echo · reverb

 split the old *Space* plugin into three, and with it went the fixed `dry · echo · room` chain, so
both effects' pages said **dry is the host's**: put them on a send. [ECHO §13](../echo/ECHO.md) left the follow-up
open — *"a user who inserts Echo on a bus rather than a send needs the input passed through somewhere"* — and
Reverb had the same gap without naming it.

**Two levels, not a mix.** `output.wet` and `output.dry`, in dB, −60 reading as **silence exactly** rather than
"very quiet". A crossfade cannot say the thing a room actually does: *the wet level is the room's and the dry
level is where the source is*. One knob forces them onto a single axis, so asking for more dry takes the room
away, which is not how a room behaves. Two levels **contain** the mix idiom — fully dry and fully wet are both
reachable — and additionally reach the case the knob cannot: the room stays put while the source walks away.

For Reverb this is a control the engine already half-models: `distance` grows the room's energy as r² against a
direct sound of 1 ([REVERB §4.4](../reverb/REVERB.md)), which was invisible because there was nothing for the room
to be heard against.

**No `mix` parameter beside them.** It would be a second source of truth for the same two gains, and automating
it would fight the faders. The UI can draw the pair on one row, which is the house idiom already
([UI §17](../suite/LOOK.md)).

**It is an OUTPUT STAGE, not a path through either algorithm.** The gains are applied after everything — after
the reflections and the tail in Reverb, and in Echo after the rings are written, so the dry echoes nothing and
the feedback is what it always was. That matters twice: the balance between reflections and tail stays
measurable, and **every measurement on either page is still a measurement of the effect**, because the dry
defaults to off and a wet of 1 with a dry of 0 is bit-for-bit what the engine always did — all 34 goldens
unmoved.

**Where they live.** In the engines, not the callers: each engine already ramps every gain it owns over a
control hop, and a second smoother outside would be a second place to get it wrong.

**Defaults:** wet 0 dB, dry off. Every send-based session behaves exactly as it did; inserting on a track means
bringing the dry up. Echo gains both keys (it had no global level at all — a tap's level is its own); Reverb's
approved `output.level` **becomes `output.wet`**, which is what it already was, and gains `output.dry` beside
it. Echo is 135 keys, Reverb 95.

**Would overturn:** wanting the familiar single knob badly enough to accept that it cannot express a fixed room
— which is the thing this was asked for.
