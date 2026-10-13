// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <vector>

#include "Controls.h"
#include "bambi/ui/ParameterPage.h"

namespace bambi::ui {

/*  The decoder's right panel: the open output's page -- headphones, speakers -- and the temporary tabs: tracking
 *  from the orientation strip, personalisation from the headphones page, the loudspeaker table from the speakers page.
 *  Every page opens with what the output is, its own settings under that, then its channel map -- the bus's
 *  channels, what plays on each, this output's own outlined -- then its options. The frame is the shared one.
 */
class DecoderPanel final : public ParameterPage {
public:
    explicit DecoderPanel(DecoderControlState& state);
    void paint(juce::Graphics& g) override;

private:
    float paintChannels(juce::Graphics& g, juce::Rectangle<float> content, float y, OutputPage output);
    void paintHeadphones(juce::Graphics& g, juce::Rectangle<float> content);
    void paintSpeakers(juce::Graphics& g, juce::Rectangle<float> content);
    void paintLoudspeakers(juce::Graphics& g, juce::Rectangle<float> content);  ///< the table, a temporary tab
    void paintTracking(juce::Graphics& g, juce::Rectangle<float> content);
    float paintPhone(juce::Graphics& g, juce::Rectangle<float> content, float y);  ///< the QR code and the steps
    float paintLearn(juce::Graphics& g, juce::Rectangle<float> content, float y);  ///< learn mode: the port's list
    void paintPersonalise(juce::Graphics& g, juce::Rectangle<float> content);
    float paintStateTile(juce::Graphics& g, juce::Rectangle<float> tile, const juce::String& name,
                         const juce::String& value, double shown, double low, double high, double step, const char* key,
                         bool enabled, std::function<void(DecoderState&, double)> set,
                         std::function<void(DecoderState&)> reset);
    void chooseLayoutFile();
    void chooseSofaFile();

    DecoderControlState& state_;
    int shownKind_{-1};  ///< the layout kind whose section is open before anything of it is loaded: 0 itu, 1 file
    juce::String calibrationNote_;  ///< what the last calibration step said
    juce::String sofaError_;        ///< why the last SOFA file was not read, until the next is
    juce::String layoutError_;      ///< why the last layout or matrix file was not read, until the next is
    std::unique_ptr<juce::FileChooser> chooser_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DecoderPanel)
};

}  // namespace bambi::ui
