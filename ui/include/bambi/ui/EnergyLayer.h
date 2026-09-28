// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <juce_graphics/juce_graphics.h>
#include <vector>

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
        the field moves every frame -- and the globe's resampling table only when the camera turns.
        False, and nothing drawn, when the field has not been prepared. */
    bool paint(juce::Graphics& g, Projection projection, const Camera& camera, const Viewport& vp,
               const EnergyField& field, float opacity);

private:
    void buildEquirect(const EnergyField& field);
    void buildGlobe(const EnergyField& field, const Camera& camera);

    juce::Image equirect_, globe_;
    std::vector<float> globeU_, globeV_;  ///< where each globe pixel samples the equirect
    std::vector<std::uint8_t> globeInside_;
    Camera tableFor_{-99.0, -99.0};  ///< the camera the globe table was built at
};

}  // namespace bambi::ui
