// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/host/Parameters.h"

#include <cmath>

namespace bambi::host {
namespace {

/// A core string as JUCE wants it.
juce::String toJuce(std::string_view s) { return juce::String::fromUTF8(s.data(), static_cast<int>(s.size())); }

const juce::String kRealMinus = juce::String::fromUTF8("\xe2\x88\x92");

/// A number as a host shows it: the interface's digits, with an ASCII minus, since some hosts and wrappers pass the
/// text on as bytes they do not read as UTF-8.
juce::String hostText(double value, int decimals) {
    return toJuce(formatNumber(value, decimals)).replace(kRealMinus, "-");
}

/// A number as a host's text field gives it back; a real minus sign, typed or pasted, read as a minus.
float numberOf(const juce::String& text) { return text.replace(kRealMinus, "-").trim().getFloatValue(); }

}  // namespace

juce::AudioProcessorValueTreeState::ParameterLayout parameterLayout(const ParamManifest& m) {
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    for (int at = 0; at < m.size(); ++at) {
        const ParamDesc& d = m[at];
        const juce::ParameterID id{toJuce(d.key), 1};
        const auto name = toJuce(d.name);
        const auto unit = toJuce(d.unit);
        switch (d.type) {
            case ParamType::Float: {
                //  The same skew the editor and remote edits use, or a position would mean one rate
                //  to the host and another to the editor.
                juce::NormalisableRange<float> range(d.min, d.max);
                if (const float centre = m.skewCentreOf(at); centre > 0.0f) range.setSkewForCentre(centre);
                //  written to the digits the interface shows, so text to value to text is stable; JUCE's own seven
                //  decimals are finer than the value the host stores
                const int decimals = displayDecimals(d);
                layout.add(std::make_unique<juce::AudioParameterFloat>(
                    id, name, range, d.def,
                    juce::AudioParameterFloatAttributes()
                        .withLabel(unit)
                        .withStringFromValueFunction(
                            [decimals](float v, int) { return hostText(static_cast<double>(v), decimals); })
                        .withValueFromStringFunction([](const juce::String& text) { return numberOf(text); })));
                break;
            }
            case ParamType::Choice: {
                juce::StringArray choices;
                choices.addTokens(toJuce(d.choices), ",", "");
                layout.add(std::make_unique<juce::AudioParameterChoice>(
                    id, name, choices, static_cast<int>(std::lround(d.def)),
                    juce::AudioParameterChoiceAttributes().withLabel(unit)));
                break;
            }
            case ParamType::Bool:
                layout.add(std::make_unique<juce::AudioParameterBool>(
                    id, name, d.def > 0.5f, juce::AudioParameterBoolAttributes().withLabel(unit)));
                break;
        }
    }
    jassert(m.byKey(kBypassKey) == kNoParam);
    layout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{kBypassKey, 1}, "bypass", false));
    return layout;
}

void Parameters::attach(const ParamManifest& m, juce::AudioProcessorValueTreeState& apvts) {
    manifest_ = &m;
    for (int at = 0; at < m.size(); ++at) {
        const auto i = static_cast<std::size_t>(at);
        raw_[i] = apvts.getRawParameterValue(toJuce(m[at].key));
        objects_[i] = apvts.getParameter(toJuce(m[at].key));
        jassert(raw_[i] != nullptr);
    }
}

void Parameters::intake() noexcept {
    //  `count()` and never `values.size()`: the values array is the shared cap and the atomics are
    //  this plugin's own length. Walking the longer one reads off the end of the shorter.
    for (int at = 0; at < count(); ++at) {
        const auto i = static_cast<std::size_t>(at);
        if (raw_[i] != nullptr) values[i] = raw_[i]->load(std::memory_order_relaxed);
    }
}

}  // namespace bambi::host
