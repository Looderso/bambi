// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <vector>

#include "../PluginProcessor.h"
#include "bambi/editor/PatchControls.h"
#include "bambi/link/link.hpp"
#include "bambi/ui/InstanceModel.h"
#include "bambi/ui/SceneStrip.h"

namespace bambi::ui {

/*  The decoder window's control state: the shared patch copy and its target, this window's own view state --
 *  which output's page is open, the temporary tabs, the scene's camera -- and the outputs as the header's tabs.
 *
 *  The outputs stand where the effects keep their instances: there is normally one decoder in a session. Each is
 *  an entry of the instance model with a fixed id, so the shared header lists them unchanged; the square in a tab
 *  that plays an output is the header's one option for this plugin.
 */
enum class OutputPage { Headphones = 0, Speakers = 1 };
enum class TempTab { Tracking, Personalise, Loudspeakers };

struct DecoderControlState final : public editor::PatchControls, public InstanceModel {
    explicit DecoderControlState(BambiDecoderProcessor& p);
    BambiDecoderProcessor& processor;

    //  ---- the outputs as tabs -------------------------------------------------------------------
    static constexpr int kOutputs = 2;
    static Uuid outputId(int index);
    int instanceCount() const final { return kOutputs; }
    Uuid instanceAt(int index) const final { return outputId(index); }
    juce::String instanceLabel(int index) const final;
    Uuid selectedInstance() const final { return outputId(static_cast<int>(page)); }
    void selectInstance(const Uuid& id) final;
    /// -1 no mark, 0 a hollow square (not playing), 1 filled (playing): the header's tab mark.
    int outputMark(int index) const;
    void toggleOutput(int index);

    //  ---- which decoder the window shows ---------------------------------------------------------
    /// The session's decoders as the header's switch lists them: this one first, then the others by their label.
    std::vector<std::pair<Uuid, juce::String>> decoders;
    Uuid shownDecoder;  ///< the one followed; this instance's own id, or zero, for itself
    juce::String selectedLabel;
    juce::String anotherLabel() const final { return remote ? selectedLabel : juce::String(); }
    void selectSelfInstance() final;

    OutputPage page{OutputPage::Headphones};
    std::vector<TempTab> temps;  ///< open temporary tabs, in the order they were opened
    int shownTab{0};             ///< 0 the page; 1.. a temporary tab
    void openTemp(TempTab t);
    void closeTemp(TempTab t);

    //  ---- the scene ---------------------------------------------------------------------------
    SceneViewState sphere;
    bool headFrame{false};  ///< the scene turned with the gaze instead of the room's frame
    bool showSources{true};
    LinkScene scene;  ///< the session's instances, for the sources the scene draws

    const DecoderState& decoder() const { return patch.decoder; }
    int renderedOrder() const final { return std::clamp(processor.engineOrder(), 1, kRegionWeightsOrder); }
};

}  // namespace bambi::ui
