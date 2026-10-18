// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <span>
#include <vector>

#include "bambi/link/link.hpp"
#include "bambi/math/vec3.hpp"
#include "bambi/scene/view.hpp"
#include "bambi/ui/OverlayMenu.h"

namespace bambi::ui {

/*  What a scene draws of a Decoder: its loudspeakers and its head. Drawn here, once, for the Decoder's own scene and for every
 *  other plugin's, which shows the session's decoders as an overlay a toggle and its list switch ([0221]): a source can be placed
 *  against the playback layout it will be heard on, with the listener's head turning as it turns. Display only -- nothing
 *  audible reads any of it.
 */

/// A loudspeaker as drawn: its direction in the frame the scene is drawn in.
struct SceneSpeaker {
    Vec3 direction;
    int channel{0};
    bool imaginary{false};  ///< takes part in the panning and plays nothing: a dashed square, no number
};

/// The layout's squares: solid for a loudspeaker, dashed for an imaginary one, a loudspeaker's channel beside it.
void paintLoudspeakers(juce::Graphics& g, Projection projection, const Camera& camera, const Viewport& vp,
                       std::span<const SceneSpeaker> speakers, float alpha);

/// The head: a ring round where it looks, and on the globe a head at the centre with its eyes and a line to the gaze; on the
/// equirect a cross at the gaze. `gaze` and `left` are unit vectors in the frame the scene is drawn in.
void paintHead(juce::Graphics& g, Projection projection, const Camera& camera, const Viewport& vp, Vec3 gaze, Vec3 left,
               float alpha);

/// The parts of a Decoder the list offers, under the Decoder itself (part 0).
inline constexpr std::uint8_t kDecoderPartSpeakers = 1;
inline constexpr std::uint8_t kDecoderPartHead = 2;

/// A row for each Decoder in the session, and under it its speakers and its head. Both are always listed, so a choice survives
/// the Decoder changing what it plays; what a Decoder does not play is not drawn.
std::vector<OverlayRow> decoderRows(const LinkScene& scene);

/// What a paint drew: for a tool that asks what the list's boxes decided.
struct DecoderOverlayDrawn {
    int speakers{0};  ///< loudspeaker squares, real and imaginary
    int heads{0};
    Vec3 gaze{};  ///< where the last head drawn looks
};

/// Every ticked Decoder's loudspeakers, while it plays them, and its head, while it plays headphones, in the room's frame.
DecoderOverlayDrawn paintDecoderOverlay(juce::Graphics& g, Projection projection, const Camera& camera, const Viewport& vp,
                         const LinkScene& scene, const OverlayMenu& menu);

}  // namespace bambi::ui
