# Decisions

Why things are the way they are, where the code cannot say it. One file per decision in [decisions/](decisions/),
each with a **Status** line: *accepted*, *revised in part by N*, *superseded by N*, or *rejected* — a rejected entry
is a dead end, kept so it is not walked again. A decision that changes gets a new number; the old one is not
rewritten.

**What earns an entry**: a reason that is not visible in the code and that someone would otherwise undo, a dead end
someone would plausibly try again, or a constraint from outside the code (a licence, a format, a release rule).
Everything else is said by the code, its tests and the concept pages. Numbers are not reused; the gaps are entries
that were removed because they no longer said anything the code doesn't — git history has them.

| entry | status | title |
|---|---|---|
| 0001 | accepted, provisional | Source width uses SH cap convolution (method A) — provisional |
| 0006 | accepted | GPLv3, open source, free |
| 0007 | accepted | Use libraries for standard algorithms |
| 0009 | accepted | A retry loop must be slower than what it retries against |
| 0010 | accepted | The scene observes paths; nothing real-time touches the segment; a source is never dragged |
| 0012 | accepted | The plugin: native JUCE UI, headless until the end, host contract first |
| 0015 | revised by 0049 | State reaches the audio thread by snapshot, never by lock |
| 0017 | accepted | Controlling another instance: its controls on the bus, its patch edited whole |
| 0026 | accepted | The link across plugins: definitions travel, values do not |
| 0029 | accepted | VST3 and CLAP only; AU is removed |
| 0030 | accepted | Features are computed locally, never carried by the link |
| 0031 | accepted | The suite stays on JUCE |
| 0035 | accepted | Core is layered, and each plugin's engine is a library inside it |
| 0038 | revised by 0043 | A bounce matches what was heard on the machine that heard it |
| 0043 | accepted | A threshold that feeds a measurement is a ramp; a latch belongs where the output is an event |
| 0044 | accepted | Cross-architecture agreement is checked by nulling two builds and by `static_assert`, not by hashes |
| 0049 | accepted | A parameter is a modulation destination only if its effect recomputes on the audio thread, in bounded work, from what `prepare` sized |
| 0053 | accepted | Echo's skew moves sources and keeps their level: no Jacobian, no scaling |
| 0058 | accepted | A parameter manifest is a value a plugin owns, not a global enum |
| 0064 | accepted | Every effect has two levels, wet and dry — not a mix |
| 0111 | accepted | A stereo input: two encoders always fed mid and side, and a side that is a difference |
| 0118 | accepted | MIDI: notes trigger envelopes, and a controller is the host's to map |
| 0123 | accepted | No backwards compatibility before the first release |
| 0129 | accepted | A step's song position comes from an anchor, not from the block; one denormal environment |
| 0139 | accepted | The direct is never louder than the input; the probe repaints with the patch; displace has a turn |
| 0165 | accepted | Personalisation is a quick gain over the KU100: one slider for the head, one for the ears, on symmetric heads |
| 0169 | accepted | The model ships learned from HUTUBS and 3D3A |
| 0170 | accepted | AirPods are read by a helper app inside the decoder's bundle, launched through Launch Services |
| 0210 | accepted | Live plays on bambi's loop device alone; the host is its sound card |
| 0211 | accepted | The host sets the suite's plugins through a control interface of their own |
| 0228 | accepted | One latency for every path, the tracker held in a bounce, the rates on the transport, and a matrix swap that is acknowledged |
| 0233 | rejected | “pffft is 3× slower than numpy” |
| 0237 | rejected | “Regions are shared definitions: a slot references a region in a session-wide library.” |
| 0240 | rejected | “Generators are transport-locked, so a copied region moves identically.” |
| 0242 | rejected | “Hysteresis is the fix for a thresholded feature decision.” |
| 0243 | rejected | “The goldens can be made architecture-independent by hashing at 24-bit.” |
| 0249 | rejected | “A plugin can read AirPods through CoreMotion, from an Objective-C++ file.” |
