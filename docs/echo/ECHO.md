# bambi Echo — loops of a field, each turning as it repeats

Echo is an effect on the ambisonic bus. It takes an (N+1)²-channel field, N from 1 to 7, and repeats it through
**four loops**. Each repeat is **turned about an axis**, optionally **slid along that same axis**, softened in
direction, band-limited, and fed back. Whatever a loop does to the field it does to all of it, so Echo needs no
idea of where a source stands.

| page | holds |
|---|---|
| this page | the model: what a repeat goes through, the four loops, time |
| [ECHO-ENGINE.md](ECHO-ENGINE.md) | the engine pass by pass, smoothing, cost, memory, buses |
| [ECHO-CONTROLS.md](ECHO-CONTROLS.md) | the send, every control, the panel, modulation, ducking |
| [ECHO-VISUALISATION.md](ECHO-VISUALISATION.md) | the strip, the energy picture, the ping |

Regions are [suite/REGIONS.md](../suite/REGIONS.md); the engine is `core/include/bambi/echo/`, the plugin is
`plugins/echo/`.

## What Echo is, and is not

- **Its own plugin.** Echoes of a room come from putting Echo after Reverb on the track, not from a fixed chain
  inside one plugin.
- **The output is the sum of the four loops, plus a dry level.** The loops have no dry path; `dry` and `wet` are
  two output levels applied after the rings are written, so neither reaches the feedback.
- **A gain by direction in front of the echo** — a hole, a spotlight, a gate — is a region on the host's track,
  in front of Echo, not a per-tap control. Echo's own region is the **send**, applied once to the input for all
  four taps ([ECHO-CONTROLS.md](ECHO-CONTROLS.md#the-send)).
- **A tap that cannot be heard is not computed.** Switched off, it fades out, then sleeps with its ring and band
  state cleared, so a woken tap starts clean. Switching off never chops a ringing tail.

## The model: a rigid motion

A repeat is moved by **a spin about an axis and a slide along that same axis**.

- The spin and the slide commute exactly, so the order they are applied in is free.
- A pure spin holds colatitude: a circle. A spin with a slide walks azimuth evenly while colatitude changes: a
  spiral. No extra machinery.
- A tap is **an axis and an angle**, and nothing more. An arc and an orbit are the same thing at different spans.
- **Trajectories that are not orbits of a rotation** (lissajous, waves, drawn curves) cannot mean anything on a
  bus: only content at one point would trace them. They belong to the encoder, where there is a source to move.
- A skew only shows when the field holds more than one direction. One source can only move; several can draw
  together or fly apart. Judge a skew with several voices.

### The slide is a latitude warp

Along the axis, **z ↦ (z + a) / (1 + a·z)**, azimuth untouched (Pomberger and Zotter, *Warping of 3D Ambisonic
Recordings*, 2011). It couples harmonics of the **same m** only: 44 pairs at order 3 where a dense matrix has 256,
344 at order 7 where it has 4,096. The matrix is the plain composition with the map, `M = (2nⱼ+1)/4π ∫ Yᵢ(Td)
Yⱼ(d) dΩ`, with no area factor and **no scaling**.

| property | value |
|---|---|
| a source at d | lands exactly at T(d), at unit gain |
| spectral radius | exactly 1 at every order and skew, so a loop at a **still** skew never diverges below 0 dB feedback |
| what truncation takes from a diffuse field | 0.48 dB a pass at ±0.9; under 0.65 dB anywhere measured |
| positive skew | gathers the field at the axis's tip; negative, at the opposite pole |
| where the axis's equator goes | colatitude arccos a: 53° at the control's ±0.6, 26° at the ±0.9 clamp |
| at ±1 | every weight is zero and the wet path is silent, so the warp clamps at ±0.9; the control offers ±0.6 |
| a point source in any pass | never above +0.4 dB (order 3, skew 0.6) |

Measured in double precision with a converged build (64 Gauss–Legendre nodes, exact to 1e-13 for |a| ≤ 0.9 up to
order 7).

**What a skew can do to a loop's level.** A coherent input can **swell before it decays**: at skew 0.6, feedback
−0.3 dB, a source at the pole the field is pushed away from rises by about 11 dB around pass 9 at order 3 and
about 16 dB around pass 6 at order 7; at −3 dB feedback the swell is +2.2 / +5.1 dB, at −6 dB none. While the axis
or the skew **moves**, the bound on a loop is the warp's largest singular value, not its eigenvalue, so a loop
whose every still setting decays can grow. The engine therefore guards the ring write
([ECHO-ENGINE.md](ECHO-ENGINE.md#the-ring-write-guard)).

**A skew never alternates.** Warps along one axis compose as Möbius maps: a then a again is 2a/(1 + a²), always
further toward the same pole. A turn composes linearly, so 180° twice is the identity, which is exactly
ping-pong. For a pattern that rises then falls, use two taps offset from each other.

**The axis goes to the pole.** Each pass carries the axis to the pole once, applies a turn about the vertical (two
multiplies a channel) and a warp (same-m coupling only), and carries the axis back. A tap whose axis is the pole
skips both frame turns.

## Four loops, each with its own feedback

A tap is a loop: *every 1/8, 45° further round, 6 dB down.*

- **One shared input line.** The send writes it; every tap reads it **at its own offset**, which is how two loops
  interleave.
- **Each tap has its own ring**, read one period behind where it is written. Repeat j is the pass applied j times
  by the loop itself, so **four taps cost four passes a sample however many repeats are audible**.
- **Repeat j** lands at `offset + j × period`, at `level × feedback^(j−1)`, turned j × spin; the skew composes j
  times; blur, order gains and band apply j times.
- **Level is outside the loop, feedback inside it.** Moving level changes what is heard now and nothing about the
  tail in flight.
- **The taps do not feed each other.** Each loop closes on itself; they share only the input.
- **Nothing is heard untransformed**: the first repeat has already had one pass.

What a loop costs is memory, which is why there are exactly four. Every ring and the input line are allocated at
the worst case at `prepare` and prefaulted, because the period is a host parameter and a ring grown on demand
would be handed over at a different sample in an offline render than live.

| order | longest tap | channels | four rings + input line, 48 kHz | at 96 kHz |
|---|---|---|---|---|
| 1–3 | 2.4 s | 16 at order 3 | 37 MB | 74 MB |
| 4–5 | 1.6 s | 36 at order 5 | 56 MB | 112 MB |
| 6–7 | 1.2 s | 64 at order 7 | 74 MB | 148 MB |

A synced tap that asks for more than its cap is **halved onto the grid**, so sixteen steps become eight and it
stays in rhythm; the step count the user set is never rewritten. One function (`maxTapSeconds`, `tapTimes`) says
what a tap gets, and the engine and the editor both call it.

## Time: synced or free, per tap

One loop free-running against three synced ones is a thing you want, so sync is per tap.

- **A tap keeps its time twice**, in sixteenths and in milliseconds; `sync` says which is live. **Leaving sync**
  writes the current time into milliseconds, so nothing moves. **Returning to sync** snaps to the nearest step.
  The offset rides the same basis.
- **Synced time is one integer, the sixteenths a pass lasts** (1…16): the straight values and the dotted ones, 3 a
  dotted eighth, 6 a dotted quarter. Triplets are the swing's: a shuffle is the same grid played unevenly.

| | synced | free |
|---|---|---|
| one step | a sixteenth: 60 / bpm / 4 s | none |
| period | `steps` × step | `time` ms, 20…2400 |
| offset | (`offset steps` + `swing`) × step | `offset` ms, 0…2400 |
| leaving sync writes | | `time` = period, `offset` = offset, both clamped |
| returning to sync writes | `steps` = nearest whole step (1…16); `offset steps` = whole part of offset/step (0…16); `swing` = the fraction (0…0.9) | |
| shown as | 1 `1/16`, 2 `1/8`, 3 `1/8.`, 4 `1/4`, 6 `1/4.`, 8 `1/2`, 12 `1/2.`, 16 `1 bar`, otherwise `n/16` | ms under a second, s above |

The engine clamps a period to 0.02 s…the order's cap and an offset to 0…the cap. A free period is held to 20…2400
ms by the parameter itself.

### Swing rides the offset, not the period

The loop stays perfectly even; what swings is **where it sits against the other loops**. Two taps half a period
apart are straight; nudge one late and the pair shuffles. A loop that swung against itself could never line up
with another. The readout says where a tap falls inside its own loop, (offset mod period) / period: 50 % is
straight, 67 % is the shuffle. It shows only when the tap is synced and its offset is not zero.

### One control for both timings

Synced or free is the suite's shared control ([VISION](../VISION.md)). It **draws both timings at once**, the one
not in use greyed and inert, so the value the snap will land on can be read before committing. Swing takes its own
row beneath the pair, greyed when the tap is free. Each tile is named for what its number is: the free side names
the quantity (`time`, `offset`), the synced side the musical unit (`steps`, `offset steps`), so no name repeats in
a tap's panel. The stored values keep the basis in their keys.

Background: decisions 0053, 0064.
