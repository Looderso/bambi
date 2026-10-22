// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <juce_graphics/juce_graphics.h>
#include <vector>

#include "bambi/math/shrotation.hpp"
#include "bambi/scene/energy.hpp"
#include "bambi/scene/view.hpp"

/*  The energy picture, as any scene view draws it: where the field's energy is, looked at through the
 *  sharpest beam its order allows. Orange for what arrived and blue for what the plugin added, mixed
 *  by the share -- so an encoder's, which has nothing arriving, is all blue under its orange source.
 *  A layer a view holds, so the encoder's scene draws the same picture by the same code.
 */
namespace bambi::ui {

class EnergyLayer {
public:
    /*  Draw `field` into the view's frame at `opacity`. The equirect texture is rebuilt every call --
        the field moves every frame -- and the globe's resampling table only when the camera or the turn
        changes. `turn`, when given, is a view drawn turned, as in a listener's frame: what is drawn in
        direction d is the field at turn * d. False, and nothing drawn, when the field has not been prepared. */
    bool paint(juce::Graphics& g, Projection projection, const Camera& camera, const Viewport& vp,
               const EnergyField& field, float opacity, const Mat3* turn = nullptr);

private:
    void buildEquirect(const EnergyField& field, const Mat3* turn);
    void buildGlobe(const EnergyField& field, const Camera& camera, const Mat3* turn);

    juce::Image equirect_, globe_;
    std::vector<float> globeU_, globeV_;  ///< where each globe pixel samples the equirect
    std::vector<std::uint8_t> globeInside_;
    Camera tableFor_{-99.0, -99.0};         ///< the camera the globe table was built at
    Mat3 tableTurn_{kIdentity3};            ///< and the turn
    std::vector<float> turnedU_, turnedV_;  ///< where each texel of a turned equirect samples the field
    Mat3 turnedFor_{kIdentity3};            ///< the turn turnedU_ and turnedV_ were built at
};

}  // namespace bambi::ui
