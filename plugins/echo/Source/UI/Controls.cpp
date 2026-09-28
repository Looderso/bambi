// SPDX-License-Identifier: GPL-3.0-or-later
#include "Controls.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "bambi/echo/control.hpp"
#include "bambi/echo/ping.hpp"

namespace bambi::ui {

EchoControlState::EchoControlState(BambiEchoProcessor& p)
    : bambi::editor::EffectControls(p, bambi::echoParams(), bambi::echoMod(), 2,
                                    {.firstTab = 1, .slots = {{0, "region1", "send"}}}),
      processor(p) {}

bambi::ParamId EchoControlState::tapParam(int tapIndex, const char* field) const {
    const auto key = "tap" + std::to_string(std::clamp(tapIndex, 0, bambi::kEchoTaps - 1) + 1) + "." + field;
    return static_cast<bambi::ParamId>(bambi::echoParams().byKey(key));
}

/*  "A tap is a loop, so its picture is a spiral -- a direction carried round the tap's axis a turn
    at a time, climbing toward the pole as the skew compounds, fading as the feedback does. The
    stroke carries the fade and every pass drops a bead as wide as its blur, so level, turn, skew
    and blur are all one drawing."

    Every enabled tap answers from the one direction the probe stands at; the selected one is drawn
    in full and the rest at a third. The flow -- twelve spokes round the axis, the path the field
    itself takes -- goes under them, because it is the reference a click is read against. */
void EchoControlState::answerProbe(bambi::Vec3 from, bambi::ui::ProbeReply& into) {
    const auto& diag = processor.diagnostics();
    bambi::EchoResolver resolver;
    //  the taps as they play, modulation included, not as they are set
    const auto played = bambi::ui::patchAsPlayed(*this);
    const auto settings = bambi::echoSettingsFrom(bambi::echoParams(), played.params, played.regions[0].shape);
    const auto frame =
        resolver.resolve(settings, diag.bpm.load(), diag.sampleRate.load(), std::max(1, processor.engineOrder()));

    std::vector<bambi::Vec3> points;
    std::vector<float> along;
    for (int i = 0; i < bambi::kEchoTaps; ++i) {
        const auto& t = frame.taps[static_cast<std::size_t>(i)];
        if (!t.on) continue;
        const bool lead = i == tap;
        const auto beads = bambi::pingSpiral(t.pass, from, 16, t.level, t.feedback);
        if (beads.empty()) continue;
        const int passes = static_cast<int>(beads.size());

        /*  The flow, once for every spoke: the same curve, turned. Only worth drawing when something
            actually moves -- a tap that neither turns nor slides carries the field nowhere. */
        if (std::abs(t.pass.spinRad) > 1e-6 || std::abs(t.pass.skew) > 1e-4) {
            std::array<bambi::Vec3, bambi::kFlowSpokes> seeds{};
            bambi::flowSeeds(t.pass, seeds);
            for (const auto& seed : seeds) {
                bambi::pingTrace(t.pass, seed, passes, points);
                into.traces.push_back({points, {}, i, lead, true});
            }
        }

        //  the spiral itself, its fade running along the stroke
        bambi::pingTrace(t.pass, from, passes, points, &along);
        std::vector<float> weight(along.size(), 1.0f);
        for (std::size_t k = 0; k < along.size(); ++k) {
            /*  The pass's real gain, read between whole passes too. Compressed the way a meter is:
                a spiral with any feedback worth using is gone after two passes otherwise, and 6 dB a
                pass leaves nothing to see the shape in. */
            const double u = static_cast<double>(along[k]) * passes;
            const double gain = t.level * std::pow(std::max(1e-9, t.feedback), std::max(0.0, u - 1.0));
            weight[k] = static_cast<float>(std::pow(std::clamp(gain, 0.0, 1.0), 0.45));
        }
        into.traces.push_back({points, weight, i, lead, false});

        for (const auto& bead : beads) into.marks.push_back({bead.direction, bead.gain, bead.blurRad, i, lead});
    }
}

}  // namespace bambi::ui
