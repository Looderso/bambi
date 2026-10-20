# Decoder: head tracking and OSC orientation

How orientation reaches the decoder and what it drives. Part of [Decoder](DECODER.md).

The decoder receives orientation over OSC and keeps it. A head, a phone or any tracker turns the scene, and the
decoder is the one place at the end of the chain where that happens: nothing is forwarded to other plugins.

## What it accepts

One standard, two formats that became standards by use, and a learn mode for everything else.

| family | addresses | form |
|---|---|---|
| **ADM-OSC** (L-ISA, Spat Revolution, Holophonix and others) | `/adm/lis/ypr` | degrees; yaw right negative, pitch up-positive: the suite's own convention |
| **SPARTA** | `/ypr`, `/quaternion` | degrees, pitch down-positive; quaternion w, x, y, z |
| **IEM SceneRotator** | `/SceneRotator/ypr`, `/SceneRotator/quaternions` | the same two forms, the same pitch |
| **Learn** | whatever arrives | the tracking tab lists what arrives on the port, each address with its numbers; a message is taken whole with its form (quaternion w first or last; yaw-pitch-roll in degrees; degrees with pitch down; radians), or three addresses are taken as yaw, pitch and roll sent apart, each its message's first number, applied once all three have spoken. The flips apply on top. It is state, so a preset keeps it; *forget* returns to the table |

Headitude (AirPods) and most hardware trackers' bridge apps send one of the first three. Phone apps (Sensors2OSC on
Android sends `/rotationvector`, w last; ZIG SIM; GyrOSC) go through learn.

- **Pitch has two signs in use.** ADM-OSC's is up-positive, as the suite's. IEM's and SPARTA's yaw-pitch-roll are
  Tait-Bryan angles about z, y, x on the same axes, so pitch turns about +y (left) and looking up is negative.
  Quaternions carry no such choice.
- **Quaternions inside.** Euler input is converted on arrival, so nothing gimbal-locks.
- **Recentring takes off the yaw only.** Pitch and roll stay against gravity, so recentring while looking down
  keeps the head looking down.
- **Applied on arrival**, not on a frame, and glided (below). The incoming rate is displayed so a sluggish feel has
  a visible cause; phones over Wi-Fi send unevenly at 50–100 Hz.
- **Local only until asked.** The receiver binds to this computer until the network is switched on; a phone needs
  it on.
- **One port, its number chosen** (default 9000, which many programs use). A taken port is said so plainly, with
  the program to look for.
- **The conversion** is `core/include/bambi/decode/orientation.hpp`: a quaternion w-first or w-last, or
  yaw-pitch-roll in degrees with either pitch sign or in radians, into the head's orientation in the world, with
  learn mode's flips, the phone's mounting mapped into the head's frame, the yaw-only recentre and the table of
  well-known addresses. Checked against the reference quaternion for every family to 0.01°. The socket (a
  `juce::OSCReceiver` on the message thread), the OSC bytes, split-address gathering, the rate readout and learn
  mode's picking stay in the plugin.

## AirPods: a helper app, no bridge

On a Mac the decoder carries a helper app in its bundle (`Contents/Helpers/`, `plugins/decoder/Helper/`) that reads
the headphones' motion with CoreMotion and sends the quaternion to the receiver on localhost.

- **Not in the plugin's process.** macOS asks the host for a motion usage text and terminates a host without one,
  which is every DAW; the helper carries its own. The permission dialog names the helper, once a machine.
- **Launched through Launch Services**, never as a child process, which would be charged to the host and crash it.
- **Started from the decoder, ended with it**: the `airpods` tab launches it with the receiver's port and ends it
  when the source changes, the decoder goes or the host exits. Nothing in the Dock, nothing to install apart from
  the plugin.
- **The helper sends `/quaternion` as SPARTA does**, so the receiver needs no new form, and
  `/bambi/helper/status` with the permission's state, which the strip shows while it starts. The helper is a target
  of the plugin build, nested into the VST3 and CLAP bundles after they are built and copied, both signed again.
- **The frame.** AirPods report a head frame: worn upright, x is the head's right, y its front, z its up
  (`Mounting::AirPods`). That is assumed until the **look-and-nod calibration** replaces it for a pair that sits
  askew: one button on the tracking tab, pressed while facing ahead and level, then again while looking down. The
  level pose yields the head's up in the sensor's frame, the nod its left axis, the front follows
  (`axesFromLookAndNod`). The axes are state, kept with the session; *forget* returns to the frame above.
- **macOS only.** Elsewhere AirPods are a phone's job, through the formats above.

## Any phone, with no app: a page

*phone* in the strip launches the helper with `--phone`; it serves a page on port 8443 to the local Wi-Fi, reached
by a QR code the tracking tab draws (qrcodegen, [THIRD-PARTY.md](../../THIRD-PARTY.md)) beside the address and
three steps. The helper says its address and state twice a second on `/bambi/helper/phone`. The page sends the
head's yaw, pitch and roll, mapped on the phone for how it is worn; the helper sends them on as `/bambi/phone/ypr`,
the suite's own form.

- **HTTPS, on a certificate the helper makes for this computer's address** (the system's openssl, held in a keychain
  file of the helper's own), because a browser gives motion data only to a secure page. The phone warns once; past
  the warning the page counts as secure. A short tutorial covers the two taps (iPhone: *Show Details → visit this
  website*; Android: *Advanced → Proceed*).
- **A WebSocket on the same address carries the data**, with POSTs as a fallback for a browser that refuses it.
- **Worn or held**: on the headphones' band, screen up and top edge forward, or held upright with the screen facing
  the listener. Both map into the head's frame on the phone; recentring is yaw only.
- **Relative orientation, not the compass**: the page reads `deviceorientation`, the gyroscope's own frame on
  Android; a compass is disturbed by the magnets in headphones and loudspeakers.
- macOS only, as the helper is.

## What orientation drives

Two turns of the field per block, each a rotation of (N+1)² × (N+1)² (`headrotation.hpp`): a 30 ms glide stepped
per 64-frame hop and crossfaded within it, so nothing snaps, the same bits however the frames are cut. Measured:
0.1 % of a core at order 3, 0.5 % at order 7 while turning.

1. **The scene's turn**, heard by every output: yaw, pitch and roll as the host has them plus their rates
   integrated while the transport runs, wrapping, the whole held at zero while *static*.
2. **The head undone**, for the binaural decode and the pair's slice only: the field turned by the inverse of the
   tracker's orientation, so what the listener hears stays in the room. An array hears the scene's turn and no
   head, since the listener's head really is in the room.

- **The tracker turns the head or the scene** (the strip's `head · scene`). As a head it is undone, as above. As the
  scene it is one more turn of the scene, heard by every output and undone by none: a performer wearing the tracker
  turns the mix for the room or for listeners on headphones.
- **The pair's slice** uses the head's orientation ([binaural](DECODER-BINAURAL.md#the-pair-fold-and-slice)).
- **Recentre** on a key.
- **An offline render takes the automation.** It renders the scene's turn as automated, sliders and rates, and holds
  the pose the tracker had when the render began: what arrives mid-render is a live value no file should bake in.
  The rates run while the transport plays, or always when `rates.retrigger` says continue; a start or a locate takes
  the integrated turn back to zero unless it says continue. The turn's target is set per 64-frame hop, counted from
  prepare, so a turning scene renders the same at every block size.

Background: decisions 0170, 0228.
