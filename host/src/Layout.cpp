// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/host/Layout.h"

#include <algorithm>
#include <cmath>

#include "bambi/math/sh.hpp"

namespace bambi::host {

int orderForChannels(int channels) noexcept {
    if (channels < 1) return -1;
    const int root = static_cast<int>(std::lround(std::sqrt(static_cast<double>(channels))));
    return root * root == channels ? root - 1 : -1;
}

int orderThatFits(int channels) noexcept {
    if (channels < 1) return -1;
    int order = static_cast<int>(std::sqrt(static_cast<double>(channels))) - 1;
    //  The square root of a perfect square can land a hair either side of the integer.
    while (numChannels(order + 1) <= channels) ++order;
    while (order > 0 && numChannels(order) > channels) --order;
    return std::min(order, kMaxHostOrder);
}

juce::String describeLayout(const juce::AudioProcessor::BusesLayout& layout) {
    const auto set = [](const juce::AudioChannelSet& s) {
        return s.isDisabled() ? juce::String("off") : juce::String(s.size()) + " " + s.getDescription();
    };
    const auto sidechain = layout.inputBuses.size() > 1 ? set(layout.getChannelSet(true, 1)) : juce::String("none");
    return "in " + set(layout.getMainInputChannelSet()) + "  |  sc " + sidechain + "  |  out " +
           set(layout.getMainOutputChannelSet());
}

bool fieldEffectLayoutSupported(const juce::AudioProcessor::BusesLayout& layout) {
    const int in = layout.getMainInputChannelSet().size();
    const int out = layout.getMainOutputChannelSet().size();
    //  Both are the bus, and both are the same order. A field in and a different field out is not a
    //  thing this effect can mean.
    const int order = orderThatFits(out);
    return order >= 0 && orderThatFits(in) == order;
}

}  // namespace bambi::host
