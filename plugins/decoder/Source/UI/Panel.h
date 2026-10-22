// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <vector>

#include "Controls.h"
#include "bambi/ui/ParameterPage.h"

namespace bambi::ui {

/*  The decoder's right panel: the open output's page -- headphones, speakers -- and the temporary tabs: tracking
 *  from the orientation strip, the loudspeaker table from the speakers page.
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
    /// The speakers page's decoding sub-tab: the pair's fold or slice, the weighting, distance compensation.
    void paintDecoding(juce::Graphics& g, juce::Rectangle<float> content, float y);
    /// The speakers page's fill sub-tab: filling what the layout does not cover, the head it uses, the tuning.
    void paintGaps(juce::Graphics& g, juce::Rectangle<float> content, float y);
    /// The listener's head -- the model, the KU100 or a SOFA file, and its two sliders -- from `y`; the y below it.
    /// One state, on the headphones page and on the speakers page's fill sub-tab.
    float paintHead(juce::Graphics& g, juce::Rectangle<float> content, float y);
    /// A labelled toggle on a flag of the state, at the row's left, its flip one undoable edit when enabled; the
    /// toggle's hit area.
    juce::Rectangle<float> paintStateToggle(juce::Graphics& g, juce::Rectangle<float> row, const juce::String& label,
                                            bool on, bool enabled, const char* edit, void (*flip)(DecoderState&));
    void paintLoudspeakers(juce::Graphics& g, juce::Rectangle<float> content);  ///< the table, a temporary tab
    void paintTracking(juce::Graphics& g, juce::Rectangle<float> content);
    float paintPhone(juce::Graphics& g, juce::Rectangle<float> content, float y);  ///< the QR code and the steps
    float paintLearn(juce::Graphics& g, juce::Rectangle<float> content, float y);  ///< learn mode: the port's list
    float paintStateTile(juce::Graphics& g, juce::Rectangle<float> tile, const juce::String& name,
                         const juce::String& value, double shown, double low, double high, double step, const char* key,
                         bool enabled, const std::function<void(DecoderState&, double)>& set,
                         const std::function<void(DecoderState&)>& reset);
    void chooseLayoutFile();
    void chooseSofaFile();

    DecoderControlState& state_;
    float mapScroll_{0.0f};  ///< how far the channel map is moved left
    int revealed_{0};        ///< the output's first channel last brought into view
    int shownKind_{-1};      ///< the layout kind whose section is open before anything of it is loaded: 0 itu, 1 file
    juce::String calibrationNote_;  ///< what the last calibration step said
    juce::String sofaError_;        ///< why the last SOFA file was not read, until the next is
    juce::String layoutError_;      ///< why the last layout or matrix file was not read, until the next is
    std::unique_ptr<juce::FileChooser> chooser_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DecoderPanel)
};

}  // namespace bambi::ui
