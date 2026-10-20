# Before release

*What has to be true before the first build ships — pulled out of [STATUS.md](STATUS.md), which is a
log and not a checklist. Nothing here is new; every line links back to where it was found or decided.
Edited in place as items close, like every other doc (rule 5).*

## Frozen the moment someone saves a project with this plugin in it

Hosts store these in sessions; changing them later breaks every project that used the plugin.

- **Plugin identity** — manufacturer and per-plugin codes, bundle/CLAP IDs. Just changed with the
  rename while there is still no migration debt to owe ([0123](DECISIONS.md));
  this is the *last* time they move for free.
- **Bus layout** — a fixed main input (mono/stereo for the encoder, (N+1)² for an effect), a
  stereo-only sidechain, ambisonic out at the highest order the channel count fits ([0029](DECISIONS.md)).
  **Validated in REAPER in both formats, 2026-09-24** — the [REAPER checklist](../.claude/skills/host-reaper-check/SKILL.md); frozen at P6.
- **Parameter keys and the state format** — a frozen key list, migration by construction; since the
  pass before the beta each key's type, range, skew and choice count are frozen with it
  (`core/tests/patch/frozen-parameters.txt`), modulation sources are saved by name, and every plugin declares
  the host's `bypass`. **Before release the list may still change, but never without your explicit go,
  change by change.**
- **The region key block** — approved 2026-09-17, in the manifest; clouds' `coverage`,
  `contrast`, `detail` and `evolve` appended to it.
- **The decoder's keys** — its parameter list (`head.yaw`, `head.pitch`, `head.roll`, `output.gain`, the shared
  blocks, then the three rates appended after them); its state keys under `decoder`
  ([patch/state.hpp](../core/include/bambi/patch/state.hpp)); the link record's loudspeaker section, which
  moved the bus version; the helper's bundle id and the names its OSC addresses carry
  (`/bambi/phone/ypr`, `/bambi/helper/...`).

## Determinism risks — all closed ([0129](DECISIONS.md))

Found by `realtime-safety` reading the code, 2026-09-17: one instance disagreeing with itself across
block sizes or host quirks, not a cross-machine issue (that's [0038](DECISIONS.md)).

| risk | state |
|---|---|
| a synced LFO's phase extrapolated from the block-start ppq | **closed** — from an anchor, in whole samples (`bambi/mod/beatclock.hpp`) |
| a tempo change **inside** a block | **decided** — taken at the first block that reports it; at most one block, the one residue |
| CLAP's sub-blocks see the **host block's** transport | **closed** — a reading that has not moved is ignored; steps use the grid's own count |
| `bambi-render` applied no input trim and accumulated ppq per step | **closed** — the same clock, and the trim; `trim-level-speed` is its golden |
| a loop wrap the host doesn't split lands the reset at a block-size-dependent place | **closed in REAPER** — it splits the block at the loop's end; passes and buffer sizes null, 2026-09-24 ([REAPER checklist](../.claude/skills/host-reaper-check/SKILL.md)) |
| the gate's threshold a bare comparison, so a level rounded differently moves it by all of it | **closed** — a 3 dB ramp |
| a host that gives no `timeInSamples` never resets | **closed** — timeline from seconds; with nothing at all, each play starts from zero |

Checked in core and in the plugin, every mutation caught ([0129](DECISIONS.md) lists them).

## Debt, deferred on purpose — pay before release

- **A two-frame pixel-diff harness for the editor** ([#12](https://github.com/Looderso/bambi/issues/12)) — deferred while the UI still changes daily,
  because a pixel harness against a moving target mostly generates churn. Two real defects so far were
  invisible to every other check (a live mark drawn unreadable; a stale foot that didn't repaint).
  Revisit once the UI stops moving.

- **A CLAP instance is labelled by its id, not its track's name, in REAPER** ([#13](https://github.com/Looderso/bambi/issues/13), found 2026-09-26). VST3 is given
  the name when an instance is created; over CLAP it never arrives, even after a rename. The wrapper now asks for
  CLAP's `track-info` once at start-up as well as on a change (bambi patch 5), which did not help -- so REAPER
  most likely does not offer the track's name through `track-info`. Next step: a settings-page row saying what the
  host sent over CLAP; if nothing, read the name through REAPER's own CLAP extension (`clap_get_reaper_context`),
  a REAPER-only path. Left open by choice: labels are cosmetic, the link works.

- **A better arrangement of Reverb's virtual speakers** ([#24](https://github.com/Looderso/bambi/issues/24), 2026-10-18). The tail's lines and the send's
  twelve virtual sources sit on a golden-angle spiral, with no mirror symmetry. A growing spot came back as dots until the
  gains were read through the region's matrix; better by ear, not perfect. The layout is the
  remaining lever, and it changes the sound, so prototype it, then listen.

## The website for the first beta

The site is its own repository (Astro); how it is built and what goes where is its own `SITE.md`, which
replaces the old site plan. The sections for the beta are `plugins · host · tutorial · research · tools ·
contribute · download · about`; foundations moved under research, the old address redirects. That skeleton, with an
outline page for each new part, is on the local branch `site-sections`, not committed (2026-10-19).

**Before the beta ships:**

- [ ] **The product page's shape**, from `prototypes/site/product-page/`: the whole window at the top, held beside
  the text and zooming in to the part being read; only the product's own parts, no spec cards; its tutorial and the
  shared window's linked at the bottom (chosen
  2026-10-19 over stepped wipes, hops and scans, which read as a page still loading). Built once as a component
  the four plugin pages and the host's page all use.
- [ ] **Pictures sharp under the zoom**: every plugin's picture tool draws at any scale (`--scale`, read in one
  place since 2026-10-19); the live host has no picture tool yet and needs one.
- [ ] **The decoder's page.** Its plugin page from DECODER.md (no measurements, no history); the
  tutorial where the header's output tabs and the orientation strip differ from the effects; pictures from
  `bambi-decoder-shot out.png [--speakers | --loudspeakers]`; the download page at four plugins, saying which
  trackers are macOS only; the decoder's references on the foundations pages from `REFERENCES.md`. The release
  pipeline's download names and the site's download page agree.
- [ ] **The host's page**: what it does, its parts, the DAWs it works with (Live today, its routing device needing
  Suite), linking status.
- [ ] **Download** for four plugins and the host, the host's install steps, the names matching the release
  pipeline's; "alpha" becomes "beta" there and on the front page.
- [ ] **Status** with Live in its table: it has been tried there (see below).

**May follow the beta** — an outline is never pushed; until it is written, its entry leaves the site:

- [x] A tutorial per plugin, covering only where each departs from the shared window, on the site's `beta`
  (2026-10-19). The host's follows once it has a picture tool.
- [ ] **The research page on personalisation**, written as a short paper with figures from the model's files.
  The model is learned from HUTUBS and 3D3A alone ([0169](DECISIONS.md)), so no SONICOM licence is needed. Waits
  for the figures to be made and for the listening round ([LISTENING.md](LISTENING.md)).
- [ ] **Thanks on about**: each person named only with their agreement.

## Needs access, not more code

See [LISTENING.md](LISTENING.md) for the ear/hardware-blocked list. **Live** has been used through the live host since
2026-10-17: saved sets, grouping, routing by hand, the bypass and the Polygon tried there (2026-10-19). **Logic** stays
untested; it has no format until AU returns as its own phase.

## Not release-blocking, tracked so it isn't lost

- **The decoder's two public issues**: the colour corrections of the array, the headphones and the loudspeakers
  ([#7](https://github.com/Looderso/bambi/issues/7)), and the decoder's state over the link bus so a followed
  decoder shows its own pages ([#8](https://github.com/Looderso/bambi/issues/8)).

- **Ableton Live's routing button needs Suite** ([LIVE-HOST](suite/LIVE-HOST.md)): it is a Max for Live device. A
  remote script would reach every edition; deferred so as not to chase Live versions while the idea is tried.

- A factory preset library, and whether Reverb's room buttons should light when the current values match a
  room — parked 2026-09-22. Presets for another instance through the link are built.
