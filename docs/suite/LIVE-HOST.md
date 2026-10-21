# The live host — Ableton Live through bambi

Live's tracks are stereo and it has no ambisonic bus between them, so the suite's chain (Encoder per source, field
effects, Decoder) cannot run inside it. The live host takes the spatial mix out of Live, plays it in the suite's own
plugins and plays the result out of the computer. Live only routes; it never carries an ambisonic signal.

```
 Live --(Live API)-- Max device ==(local TCP)== host ==(link bus)== plugin windows
 Live tracks and Main -> loop device -> host -> Decoder outputs and Main, delayed to match -> chosen devices
```

Code: `apps/live-host/` (the host), `platform/` (devices, loop device, local server), `driver/` (the
loop device), `core/include/bambi/live/` (protocol, plan, field, pairs, io, timeline, midi), `apps/live-host/device/`
(the Max device and its scripts). Rules: [apps/live-host/AGENTS.md](../../apps/live-host/AGENTS.md).

## The parts

| part | is | knows | is told |
|---|---|---|---|
| **Max device** | a Max for Live audio effect on Live's Main (`apps/live-host/device/bambi-live.amxd`) | tracks, groups, returns, names, colours, sends, faders, mute, solo, routing, tempo, transport, arrangement and session clips | each track's channel pair; which returns are silent; the host's state to save |
| **Host** | a JUCE app (`apps/live-host/`), loading the installed VST3 plugins | the sources, the field, inserts, Decoders, the audio setup, lanes | the tree, values, transport and arrangement |
| **Loop device** | a BlackHole fork named *bambi* (`driver/`), 64 channels, ids of its own | its channels and their names | names, set by the host through the custom property `bnam` |
| **Plugins** | Encoders, Echo, Reverb, Decoders, as in any DAW | their state | track name and colour (`updateTrackProperties`), the playhead, state, MIDI |

Live is the truth for tracks and their names, colours, sends and faders; the host for encoding, pairs and effects. The device applies the host's routing and decides nothing. Names and colours reach plugins as
a DAW hands them.

| between | over | carries |
|---|---|---|
| device and host | TCP to `127.0.0.1:9461`, JSON lines | the messages below |
| host and loop device | Core Audio property `bnam` | channel names |
| host and plugins | the plugin interface | track properties, playhead, editor, state, MIDI, the control interface (order, layout) |
| plugin and plugin | the link bus | as today |

TCP, not UDP: it keeps order and drops nothing, and the host's state is larger than a datagram. The server
(`platform/net`) takes one peer and answers a second with a busy line; a send waits at most 50 ms and a line is at most
1 MB, either one dropping the peer.

## The protocol

Every message is one JSON object on a line with a `type` (`core/include/bambi/live/protocol.hpp`). Reading is
tolerant: a missing field takes its default, an unknown field is ignored, an unknown type or a non-object line is
refused. Levels are dB as Live shows them, silence is null. `kProtocol` (now 3) is bumped whenever a message or its
meaning changes; a device in another protocol is welcomed once, in the host's, then neither answered nor acted on,
and its status line says so. The host logs its connection to `~/Library/Logs/bambi-live-host.log`.

**Device to host**

| type | when | carries |
|---|---|---|
| `hello` | on connecting | protocol, Live's version, rate |
| `bambi` | the route button | on or off |
| `tree` | bambi on; tracks added, removed, moved, grouped; a routing changed by hand | every track, group and return: id, kind, `parent` (where its audio goes), `group` (where Live files it), name, colour, sends, fader, pan, mute, solo |
| `value` | every observed change, automation included | track id, what (`send:<n>`, `fader`, `pan`, `mute`, `solo`, `name`, `colour`, `routing`), value |
| `transport` | play state or tempo changes, and at the rate Live reports the song position | playing, quarter notes, bpm |
| `arrangement` | bambi on, a change, `refresh` | clips and session slots per track, scenes, locators, loop, read cost |
| `slotPlay` | a track's playing slot changes, and as the clip moves | track, slot, position, loop bounds, whether the session overrides the arrangement |
| `restore` | after `hello`, when the set holds a saved state | the state as last saved |
| `routed` | after a `route` | per track: done, or why not |
| `show` | *open host* | the host's window comes forward |

**Host to device**

| type | when | carries |
|---|---|---|
| `welcome` | answering `hello` | protocol; whether the loop device was found |
| `route` | at bambi on; on every routing change | per track its pair, or back to its remembered routing; the returns to silence in Live |
| `save` | the host's state changed, at most once a second | the state, kept in the device's dict so the set saves it |
| `info` | after every routing, and on change | latency, rate, a warning per track |
| `createReturn` | a bambi send made on the routing page | the name for a new Live return |
| `command` | transport on the host | play, stop, continue, locate (quarter notes) |
| `refresh` | the timeline is shown, and now and then while Live plays | none |
| `status` | the device should show something | ready, or what is missing |

A line longer than a Max symbol (32767 bytes) crosses Max in pieces of at most 8000 characters, marked `+` and
joined at `.`.

**Persistence in the device.** Track keys, remembered routings and the host's state live in an unnamed `dict` that is a
Live parameter (`@parameter_enable 1`, `@embed 0`, blob, stored only), which Live saves with the set. Live's ids last
only while the set is open, so each track gets a key kept with its features (name, place, colour, kind, group, device
names); on reopening the most alike pairs match first, a tie or weak likeness is left new (`test-match.js`).

## What happens when

1. **The device loads.** Its Node script connects; if nothing answers it starts the host and tries for ten seconds.
   `hello`, `welcome`, `restore` if the set holds a state. It shows *ready*, or what is missing.
2. **bambi on.** `tree`; the host chooses the encoding (the restored one, else every top-level track), assigns pairs
   (`assignPairs`) and sends `route`. The device remembers each track's routing, applies the new one, mutes the
   returns the field plays, and answers `routed`. **Off:** every remembered routing is restored and what the device
   made is deleted; the host keeps its state and goes quiet.
3. **Changes and saving.** A track added or regrouped sends a new `tree` (a new top-level track is encoded by
   default); a routing changed by hand makes that node "left in Live". `save` carries every change of the host's state,
   since Live tells a device nothing when it saves; `restore` returns it on reopening.
4. **Failure.** The host dies: the device starts it once, and if it is not back within five seconds turns bambi off,
   restoring every routing. Live quits: the host quits a minute after its last device has gone. No loop device: `welcome` says so and nothing is routed. A second device: it is told
   the line is busy.

**The device in Live** shows *route* and *open host* with a connection light, the list of every track and return with
its pair, group or "in Live" and warnings, and a routing snapshot (*save routing*, *restore saved* on a confirming
press). A track on the loop device that this device did not route is shown, never moved. While route is on it keeps a
*bambi track* (listening to Ext. In 1/2, monitoring In, −12 dB, never sent to the host) for recording the binaural mix.
Moving tracks while bambi is on stalls Live 90 to 160 ms per move.

## The host's shape

One fixed shape, compiled in core (`graph.hpp`, `field.hpp`) and played by the host's own runner (`Runner.*`), not JUCE's
`AudioProcessorGraph`: send levels change every block and a rebuild must not click. The host keeps every plugin's state,
the shape and the lanes in the device's dict, so in the Live set; audio settings are in `live-host.json`. A missing plugin shows as missing, its state kept.

- **Sources.** A track bambi plays arrives on its own stereo pair and gets an Encoder; an input of the chosen
  interface (a microphone) is also a source, heard as soon as it arrives. A source past the loop device's pairs is left out.
- **Wires.** Every node plays along wires into a bus, a return, Main or one Decoder's own mix. A group is a bus (the field is a tree) or is left whole in Live; a group is taken whole or split,
  never both with its tracks. A split group's fader is its bus level.
- **Inserts.** Any part of the field, and Main, can carry a chain of Echo and Reverb, each with an id and state of its
  own. A chain whose track Live no longer has is shown as orphaned, removable, and attachable whole to another part.
- **Returns.** Each return has a mode: *live* (it stays in Live, a stereo return with Live's effect playing to Main
  dry; the engine never encodes a return) or *bambi* (a bus in the field with bambi's chain). Every node sends its field
  to a bambi return at its own send knob's level in Live, automation included; the device mutes the Live return and
  gives its own mute back after. A send from a node left in Live is lost, and a Live effect on a bambi return does nothing.
- **Sidechain.** An Encoder or insert may hear another source's stereo pair; a lost source is silence.
- **One order, 1 to 7**, for every plugin (VST3 stops at 7th); a new order loads every plugin again with its state.
  The Decoder's bus is 61 channels.
- **Latency alignment.** The plan delays every input of every sum to the slowest and holds all Decoders' mixes to one
  time. A swap of program fades what goes and what comes and waits for its lines to empty; send levels are taken on a
  256-sample grid with a ramp. Nothing in a block allocates, frees or locks.
## The window

A rail and a page. The rail lists the plugins' windows at the top and the host's own pages at its foot — I/O,
routing, timeline — over the status (Live's link, the sources, the cost). Above them, beside every page, is Live's
transport: play, filled while Live plays, and stop, which pressed again while stopped goes back to the song's start, as
Live's own does. The space bar plays or stops on every page.

## Routing page

Three columns with wires: Live's tracks and groups plus the host's inputs; returns and Main; the Decoders. A node's line holds its marker (live, bambi, bus, in group), "+ encoder" or the
Encoder's chip, its inserts (click opens, drag reorders), "+" for inserts, and "send": pressed, it rings what it may
reach and a click wires it at 0 dB; wires open under the chosen node as chips, dragged for level. The set opens with
everything in Live; *route all* and *all to Live* sit in the band. A sidechain is picked by arming a slot
(`ui::Armed`) and clicking a source. The three columns always fit the page: the gutters where the wires run narrow
until they do, so every Decoder and its cross stay in view; more Decoders than the page's height holds scroll down.

## Outputs and the I/O page

**Live plays on bambi's loop device alone.** Choosing "bambi" as Live's audio device is the whole setup. Live's Main
goes to the pair 1/2; track pairs follow: pair p is channels 2p+3 and 2p+4 (1-based), 31 pairs on the 64-channel
device. Live sends only the pairs enabled in its Output Config. The count is fixed because Live restarts the device when channels change.

**The host is the sound card.** It opens a private aggregate (seen by no other program, removed on quit) of the loop
device and the interfaces the user chose, the first the clock and the rest drift corrected. The loop device is found by
name (the one Live's aggregate holds, else *bambi*, else BlackHole 16ch), never the system default. Live's Main and the
field leave on the same devices, differing only by the plugins' latency, which the host knows. Without the host, Live
is silent; the device says "no host: no sound".

The **I/O page** has four tabs:

| tab | holds |
|---|---|
| interface | the devices (channels, reported latency; Bluetooth listed, not offered), the buffer (64), the rate (Live's; shown, never set), the order, the measured load |
| outputs | Live's Main on a pair; each Decoder's outputs placed as blocks on a device channel; copies (same signal elsewhere, or back to the bambi track); clashes marked |
| inputs | each source of the field from any device, placed on a channel |
| midi | the MIDI inputs heard, and the lead Live's MIDI is held back by |

Which outputs play, their layout and level are each Decoder's own; the I/O page places them. Outputs that do not come
back into Live are not rendered by Live; the page says so beside them.

## Timeline, transport and MIDI

The timeline is a rail view: a band with the song position as heard, tempo, the cost of Live's last
arrangement read and an *arrangement | session* switch; the tree on the left; a panel for the chosen row with its lanes.

- **Transport.** Play, stop, continue and locate go to the device as `command`; Live stays the clock. The playhead
  drawn is the one heard: the projected position (`TransportProjector`, `transport.hpp`) less the samples from the
  plugins to the speakers. The device stamps each transport message with the time it read Live's position; the host
  takes that stamp onto its own clock and places the message on the audio thread's own samples at the next block
  (`arrival.hpp`), so a message held up on its way is not taken for a locate.
- **Arrangement view.** Per Live track a row of its clips (read-only named blocks) under bars, locators and loop; then
  Main, its chain and the Decoders. Zoom about the pointer down to a bar; scroll with shift and the wheel.
- **Session view.** Live's grid with no time: a column per track, a row per scene, the playing clip marked; a chosen
  clip opens in a pane with its own ruler and clip lanes.
- **Lanes.** One parameter of one instance, in plain units by stable key, kept with the set. A click puts a point
  (snapped to beat, locators, clip edges), a drag moves it (shift: no snap), a double click removes it; a lane whose
  instance does not play is kept, greyed. The runner drives lanes per piece on the audio thread, only when the value
  moved. *Song lanes* follow the song position and wait while their track plays a session clip, until Back to
  Arrangement. *Clip lanes* belong to a clip (track, slot, name) and follow its position, looping with it.
- **MIDI.** Every instance hears all of it and answers to the channel and note it learned. Hardware: every system
  input; a note plays one buffer after it arrived. Live: a virtual port "bambi"; a MIDI track's *MIDI To* is set to it
  by hand and its *Track* switch turned on once in Live's preferences. Live's MIDI arrives ahead of its audio, so the
  host holds it back by a lead (at most 250 ms) in this computer's settings; the midi tab measures it against a click
  on Main and offers the median. Dropped MIDI lets every note go.
- The host edits only its own lanes, never Live's set. Only 4/4 is supported.

## Latency

Measured (Live 12.4): Live's part of the round trip is 9.4 ms at a 64-sample buffer and
28 ms at 128, with no drift over 20 minutes. Live's Main is delayed by the field's latency so tracks left in Live align
with the field; of the suite's plugins only the Decoder reports latency, its 64-sample partition. The song position
arrives about every 13 ms while playing (95 % within 21 to 30 ms); the host projects it forward, so time is not
sample-accurate.

## Limits

- The device needs Max for Live (Suite, or Standard with the add-on); macOS only.
- For playing a set: tracking adds the host's latency, and Live's export lacks the spatial mix.
- Main is stereo: a speaker decode plays from the host to the interface, past Live's master meter and effects.
- The host is signed with a fixed identity; macOS asks for the microphone again for every new signature.

Background: decisions 0210, 0211.
