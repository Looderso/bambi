# Host checks — REAPER

A person's checklist, run before a release and after a change to buses, layouts, parameter intake or the
transport.

What no headless check can answer: how REAPER negotiates buses, sizes a sidechain, splits blocks and renders. About
30 minutes. Each check says what passes and what to send back. Read the plugin's values off the **settings page's
`host` rows** (input, sidechain and output channels, layout mismatches, the top layout request, block size).

## Before you start

1. Rebuild and install, then **quit REAPER and reopen it**: it keeps binaries loaded, and an open project does not
   pick up a rebuilt plugin (`pgrep -il reaper`).
2. Preferences › Plug-ins › VST and CLAP: **re-scan**.
3. A **48 kHz** project and an audio file of ten seconds or more.

## Checks

1. **Loads** in VST3 and CLAP. Pass: the window opens.
2. **Ambisonic channel counts.** REAPER tracks have only even channel counts, so a plugin runs the highest order that
   fits and leaves the rest silent: 10 channels is order 2 plus 1 unused. Set the track to 4, 10, 16, 26, 36, 64
   (orders 1, 2, 3, 4, 5, 7; VST3 follows the track, CLAP offers a port configuration per order, `Ambisonic order N
   (M channels)`). Pass: output shows the track's count and the order, `layout mismatches` stays 0.
3. **Stereo input arrives.** Pass: input reads 2 ch, the input peak moves, the output peak is not `-inf`.
4. **Sidechain (encoder).** A second track sends 1/2 to channels 3/4 of a 16-channel encoder track. Pass: VST3 input
   2 ch, sidechain 16 ch (REAPER sizes it to the track), output 16 ch, and a sidechain feature moves in the matrix;
   at 4 and 10 channels output still follows the track. CLAP with *Ambisonic order 3*: input 2 ch, sidechain 2 ch.
5. **Sidechain (effects).** Their main input is the field, (N+1)² wide, and the sidechain comes after it. Encoder on a
   16-channel track A, send A → B 1–16, effect on an 18-channel track B, a third track sends 1/2 to B 17/18. Pass: the
   sidechain tab's `level` moves with the third track and a negative `level` → `wet` cell ducks. A bus width is not
   proof that audio arrives; only the matrix proves it. Never test on one track: an encoder on an 18-channel track
   writes silence over 17/18.
6. **MIDI reaches an audio effect.** A MIDI item on another track, sent audio none, MIDI all to all (or the virtual
   keyboard with the track armed). Pass: the MIDI counter climbs and an envelope's learn catches a note.
7. **The source moves.** Speed 90, play. Pass: azimuth and elevation keep changing and the source restarts once per
   play.
8. **A bounce is identical to realtime.** Order 3, speed 90, width 30. Render the track as WAV 32-bit float 48 kHz,
   multichannel files on, three times: Full-speed Offline twice, Online Render once. Then
   `./build/bambi-nulltest offline-1.wav offline-2.wav` and `... offline-1.wav online.wav`. Pass: both print
   `IDENTICAL`. `IDENTICAL AFTER A SHIFT of N frames` is latency or pre-roll, not a failure; `DIFFERENT` is the failure
  .
9. **State survives save and reload.** Change speed and width, save, quit, reopen. Pass: values return and the source
   moves as before.
10. **Automation survives the bounce.** Hosts hand over one value per parameter per block, so a render's result can
    depend on where blocks fall. Draw a speed envelope (ramps and jumps) on a VST3 track and a CLAP track, render
    offline and online as in check 8, null each pair. Pass: offline against offline and offline against online are
    `IDENTICAL`. `DIFFERENT` means automation must reach the plugin at its sample offset; send both outputs and the
    block size.
11. **A loop's end splits the block.** Encoder with speed 0, LFO 1 free, 1.7 Hz, restart, routed to yaw at full depth;
    loop 1.000 s to 2.337 s. Record output (multichannel) for three passes at block size 64, then 1024. Null pass 2
    against pass 3, and the 64 run against the 1024 run. Pass: all `IDENTICAL` (or shifted by latency). A difference
    means REAPER does not cut the block at the loop's end and a restart lands late.
12. **Stereo input modes (encoder).** REAPER's tracks are at least stereo, so a mono item is L = R and all three modes
    are live. With L = R, mid/side sounds exactly like sum and stereo does not (one signal at two points). Spread 0 is
    sum; folding the output to mono keeps the side's content; switching mode while playing is a ramp, not a click.
13. **Ducking and release (effects).** A percussive source; on the features tab drag the `level` cell of the `wet` row
    down to about −25 %. Pass: wet drops while the input plays and returns between hits; `release` 20 ms pumps back
    at once, 2000 ms stays down through gaps; when the input falls to digital silence the wet still returns at the
    release's pace. In the encoder, a source moved by level coasts to a stop; `level.release` 20–50 ms is the quick feel.
14. **The decoder on a real bus.** Tracks of 18, 36 and 64 channels: the output pins offered match, the headphones'
    pair lands on the pins the page names, a layout from channel 3 lands on 3 and up, no pin is silent. A 128-channel
    track: the decoder refuses the layout whole and REAPER falls back (note which width). Headphones on 13–14 beside
    a 7.1.4 array on 1–12: an impulse through both lands on the same sample after latency compensation. An offline
    bounce with a tracker chosen holds the render-start pose and nulls twice. Two decoders: the header's switch lists
    both and the other's pages grey. The head-tracker helper asks once for permission and is found again after a
    REAPER restart.

## What to send back

One line per check, pass or fail, with what it asks for; every `bambi-nulltest` output in full. Note anything surprising. Record a result as a dated line in `docs/RELEASE.md` or
`docs/LISTENING.md`, not here.
