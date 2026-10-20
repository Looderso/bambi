# 0170 · AirPods are read by a helper app inside the decoder's bundle, launched through Launch Services

**Status:** accepted

*2026-10-07. A tip from Daniel Rudrich, who wrote Headitude: CoreMotion can be called from a plugin through an
Objective-C++ file. He had not tried it in a plugin. Two spikes in REAPER settled it; the owner, on the second:
"it feels smooth, write it up".*

**A plugin cannot read AirPods itself.** `CMHeadphoneMotionManager` asks the privacy daemon, and the daemon checks
the requesting process's `Info.plist` on disk for `NSMotionUsageDescription`. Inside a DAW that process is the
host; REAPER and Ableton Live carry no such key, and on the first request macOS terminated REAPER (termination
namespace TCC). Nothing in the plugin's process changes the outcome: the status stays `notDetermined` until the
request, the daemon reads the file, not the loaded bundle, and a host's plist cannot be edited without breaking
its signature. Of the applications on the build machine, only Headitude and Shortcuts carry the key.

**The decoder bundles a small helper app that has the key.** It has no window and no Dock icon (`LSUIElement`),
reads the motion and sends the quaternion to the decoder over OSC on localhost
([DECODER §7.1](../decoder/DECODER-HEAD-TRACKING.md)). It must be started through Launch Services (`NSWorkspace`): a child
process is charged to its parent, the host, and would crash it the same way. Started so, its parent is launchd,
the permission dialog names the helper, and the data flowed. The spike measured 0.7 % of a core and 15 MB for
the helper, and felt smooth over localhost UDP. It quits when asked, when the plugin goes, or within half a
second of the host's exit.

**Contingent on:** macOS keeping Launch Services launches their own responsible process, and the helper signed
with a stable identity so the permission is asked once a machine. **Would overturn:** a host that ships the key
(the helper is then still the one path that works in every host), or Apple giving plugins their own permission.
