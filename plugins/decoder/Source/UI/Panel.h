// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <vector>

#include "Controls.h"
#include "bambi/ui/ParameterPage.h"

namespace bambi::ui {

/*  The decoder's right panel: the open output's page -- headphones, speakers, lens -- and the temporary tabs the
 *  head strip and the headphones page open, tracking and personalisation. Every output's page carries its channel
 *  map: the bus's channels, what plays on each, this output's own outlined. The frame is the shared one.
 */
class DecoderPanel final : public ParameterPage {
public:
    explicit DecoderPanel(DecoderControlState& state);
    void paint(juce::Graphics& g) override;

    //  ---- for tools -----------------------------------------------------------------------------
    juce::Rectangle<float> layoutButton(int which) const {  ///< 0 load, 1 itu, 2 type
        return which >= 0 && which < 3 ? layoutButtons_[static_cast<std::size_t>(which)] : juce::Rectangle<float>{};
    }
    juce::Rectangle<float> ituCell(int index) const {
        return index >= 0 && index < static_cast<int>(ituCells_.size()) ? ituCells_[static_cast<std::size_t>(index)]
                                                                        : juce::Rectangle<float>{};
    }
    bool ituPickerOpen() const { return ituOpen_; }

private:
    float paintChannels(juce::Graphics& g, juce::Rectangle<float> content, float y, OutputPage output);
    void paintHeadphones(juce::Graphics& g, juce::Rectangle<float> content);
    void paintSpeakers(juce::Graphics& g, juce::Rectangle<float> content);
    void paintLens(juce::Graphics& g, juce::Rectangle<float> content);
    void paintTracking(juce::Graphics& g, juce::Rectangle<float> content);
    float paintPhone(juce::Graphics& g, juce::Rectangle<float> content, float y);  ///< the QR code and the steps
    void paintPersonalise(juce::Graphics& g, juce::Rectangle<float> content);
    float paintStateTile(juce::Graphics& g, juce::Rectangle<float> tile, const juce::String& name,
                         const juce::String& value, double shown, double low, double high, double step, const char* key,
                         bool enabled, std::function<void(DecoderState&, double)> set,
                         std::function<void(DecoderState&)> reset);
    void chooseLayoutFile();
    void chooseSofaFile();
    std::vector<int> channelsOf(OutputPage output) const;

    DecoderControlState& state_;
    std::array<juce::Rectangle<float>, 3> layoutButtons_{};
    std::vector<juce::Rectangle<float>> ituCells_;
    bool ituOpen_{false};
    juce::String calibrationNote_;  ///< what the last calibration step said
    juce::String sofaError_;        ///< why the last SOFA file was not read, until the next is
    std::unique_ptr<juce::FileChooser> chooser_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DecoderPanel)
};

}  // namespace bambi::ui
