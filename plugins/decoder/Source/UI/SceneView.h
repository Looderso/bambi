// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <vector>

#include "Controls.h"
#include "bambi/decode/allrad.hpp"
#include "bambi/scene/view.hpp"
#include "bambi/ui/HitArea.h"
#include "bambi/ui/SceneStrip.h"
#include "bambi/ui/Widgets.h"

namespace bambi::ui {

/*  The decoder's scene, one picture in the room's frame: the session's sources, the layout's loudspeakers and a
 *  head that turns. What plays decides what is full ink -- the head while headphones play, the loudspeakers while
 *  the layout plays or headphones listen through it -- not what is drawn; the hull the layout pans across is drawn
 *  while the speakers' page is open. A `head frame` toggle turns the view with the gaze. The globe and the equirect
 *  are two of these, as every scene row in the suite is.
 */
class DecoderSceneView final : public HitArea {
public:
    DecoderSceneView(DecoderControlState& state, Projection projection);
    void setRelayout(std::function<void()> relayout) { relayout_ = std::move(relayout); }
    void paint(juce::Graphics& g) override;

    juce::Rectangle<float> presetArea(int index) const {
        return index >= 0 && index < 3 ? strip_.presets[static_cast<std::size_t>(index)] : juce::Rectangle<float>{};
    }
    juce::Rectangle<float> headFrameToggle() const { return headFrameToggle_; }
    juce::Rectangle<float> sourcesToggle() const { return sourcesToggle_; }

private:
    Viewport viewport() const { return sceneViewport(projection_, getWidth(), getHeight()); }
    Vec3 toView(Vec3 p) const;  ///< the room's frame, or the head's when the toggle is on
    void paintGraticule(juce::Graphics& g, const Viewport& vp);
    void paintHull(juce::Graphics& g, const Viewport& vp, float alpha);
    void paintSpeakers(juce::Graphics& g, const Viewport& vp, float alpha);
    void paintSources(juce::Graphics& g, const Viewport& vp);
    void paintHead(juce::Graphics& g, const Viewport& vp, float alpha);
    void strokeArc(juce::Graphics& g, const Viewport& vp, Vec3 a, Vec3 b, juce::Colour colour, float width,
                   bool dashed);
    void refreshDesign();  ///< the layout's design, copied from the processor when the layout changed

    DecoderControlState& state_;
    Projection projection_;
    std::function<void()> relayout_;
    SceneStripAreas strip_{};
    juce::Rectangle<float> headFrameToggle_, sourcesToggle_;
    std::vector<ScreenSegment> segments_, lineSegments_;
    std::vector<juce::Point<double>> skipped_;
    std::vector<Vec3> arc_;
    Mat3 head_{kIdentity3};              ///< the head the decode undoes
    Mat3 scene_{kIdentity3};             ///< the scene's turn, which the sources follow
    std::vector<Loudspeaker> designOf_;  ///< the layout `design_` was built for
    SpeakerDesign design_;
    juce::Point<float> dragLast_{};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DecoderSceneView)
};

}  // namespace bambi::ui
