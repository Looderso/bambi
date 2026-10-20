# apps/live-host/ — Ableton Live through bambi

The app that plays a Live set's tracks through the suite's installed plugins, as Live's sound card. It has its own JUCE
(built to host VST3) and sits above every library of the suite: nothing includes it. Its device and loop layer is
`platform/`, the loop driver `driver/`, the messages `core/include/bambi/live/protocol.hpp`. Concept:
`docs/suite/LIVE-HOST.md`.

## Working here

- **The protocol has a version** (`kProtocol`): any change to the messages between the Live device and the host bumps
  it. A host meets a device in another protocol once, then ignores it.
- **The host decides what plays in bambi** and routes it; plugins place their outputs on their own bus and know nothing
  of the host.
- It hosts the suite's plugins as any host would — through their formats, never by linking them.
- `Check/RunnerCheck.cpp` is the headless check; `Check/Shot.cpp` the picture tool.
- macOS asks for the microphone again for every new signature, so the app is signed with a fixed identity.
