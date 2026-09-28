// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

#include "PluginProcessor.h"
#include "UI/FrameTrace.h"
#include "UI/SceneModel.h"
#include "bambi/ui/Header.h"

namespace bambi::ui {

/*  The host-contract readout, reachable from the settings glyph, and the scene's timing trace,
    started by clicking its line. */
class DiagnosticsView final : public juce::Component {
public:
    DiagnosticsView(BambiEncoderProcessor& processor, FrameTrace& trace, std::function<void()> onRecord);
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;

private:
    BambiEncoderProcessor& processor_;
    FrameTrace& trace_;
    std::function<void()> onRecord_;
    juce::Rectangle<float> traceLine_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DiagnosticsView)
};

}  // namespace bambi::ui
