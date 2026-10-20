# 0249 · Rejected: “A plugin can read AirPods through CoreMotion, from an Objective-C++ file.”

**Status:** rejected

*Found false by 2026-10-07.* A dead end, recorded so it is not walked twice.

A tip, tried
2026-10-07: macOS terminated REAPER on the first request, because the privacy daemon checks the
*host's* `Info.plist` for a motion usage text, and no DAW carries one. The AirPods are read by a
helper app the decoder launches through Launch Services ([0170](0170-airpods-are-read-by-a-helper-app-inside-the.md)).
