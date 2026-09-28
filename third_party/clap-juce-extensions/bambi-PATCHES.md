# bambi's patches to clap-juce-extensions

Vendored from https://github.com/free-audio/clap-juce-extensions at `9fbefae`
(`Call releaseResources() in deactivate() (#188)`), with its `clap` (1.2.7) and `clap-helpers`
submodules. MIT, see `LICENSE.md` here and the `LICENSE` files under `clap-libs/`.

Vendored rather than a submodule because it carries these patches. Every change is marked
`bambi patch` in the source. They are generic — nothing bambi-specific — so they can go
upstream, where the wrapper marks the gap itself: `@TODO: implement CLAP_PORT_SURROUND and
CLAP_PORT_AMBISONIC`.

## Why

Upstream reports every audio port from the bus's *default* layout, asserts that it is mono or
stereo, and for any other layout never assigns `port_type` — an uninitialised pointer handed to
the host. It implements no port configurations. So a JUCE plugin with an ambisonic output
cannot describe it over CLAP, and a host cannot choose its order. CLAP is the only plugin format
that can carry ambisonics above 7th order, so this is required, not cosmetic.

## What

1. **A port describes the bus's current layout**, of any size, not its default and not only mono or
   stereo, so a host-selected port configuration is what the port says.
2. **Every port has a type**, and an ambisonic port is declared as one: mono, stereo, ambisonic, or
   `nullptr` ("unspecified", which CLAP allows).
3. **Port configurations are forwarded** to the processor when it offers any
   (`clapAudioPortsConfigCount / Get / Select`).
4. **`clap.ambisonic`** (and its draft id), which clap-helpers does not implement, is answered through
   the helpers' `extension()` fallback: ACN ordering, SN3D normalisation. Nothing in clap-helpers is
   modified. A processor declares a bus ambisonic with `clapAmbisonicOrderForBus(isInput, bus)`;
   above 7th order JUCE has no named ambisonic layout, so the wrapper cannot tell otherwise.
5. **The track's info is asked for once at `init()`.** The wrapper answered only
   `track-info.changed()`, and a host reports only a change: an instance put on a track that
   already had a name never learned it.

The API is in `include/clap-juce-extensions/clap-juce-extensions.h`, the changes in
`src/wrapper/clap-juce-wrapper.cpp`.

## Updating

Re-vendor the new upstream commit, re-apply the changes marked `bambi patch`, and check first
whether upstream has implemented ambisonic ports itself.
