// SPDX-License-Identifier: GPL-3.0-or-later
#include "Controls.h"

#include <algorithm>

namespace bambi::ui {

namespace {
constexpr std::array<const char*, DecoderControlState::kOutputs> kOutputNames{"headphones", "speakers"};
}  // namespace

DecoderControlState::DecoderControlState(BambiDecoderProcessor& p)
    : editor::PatchControls(p, decodeParams(), decodeMod(), 1), processor(p) {}

Uuid DecoderControlState::outputId(int index) {
    Uuid id;
    id.bytes[0] = static_cast<std::uint8_t>(0xd0 + std::clamp(index, 0, kOutputs - 1));
    id.bytes[15] = 1;
    return id;
}

juce::String DecoderControlState::instanceLabel(int index) const {
    return index >= 0 && index < kOutputs ? kOutputNames[static_cast<std::size_t>(index)] : "";
}

void DecoderControlState::selectInstance(const Uuid& id) {
    for (int i = 0; i < kOutputs; ++i)
        if (outputId(i) == id) {
            page = static_cast<OutputPage>(i);
            shownTab = 0;
            notify();
            return;
        }
    if (id == processor.identity().instance) selectSelfInstance();  // the header's home: back to this instance
}

void DecoderControlState::selectSelfInstance() {
    shownDecoder = processor.identity().instance;
    notify();
}

int DecoderControlState::outputMark(int index) const {
    switch (static_cast<OutputPage>(index)) {
        case OutputPage::Headphones: return decoder().headphonesPlay ? 1 : 0;
        case OutputPage::Speakers: return decoder().speakersPlay ? 1 : 0;
    }
    return -1;
}

void DecoderControlState::toggleOutput(int index) {
    if (remote) return;  // another instance's outputs are its own window's to play
    switch (static_cast<OutputPage>(index)) {
        case OutputPage::Headphones:
            processor.playOutput(BambiDecoderProcessor::Output::Headphones, !decoder().headphonesPlay);
            break;
        case OutputPage::Speakers:
            processor.playOutput(BambiDecoderProcessor::Output::Speakers, !decoder().speakersPlay);
            break;
    }
    refresh();
    notify();
}

void DecoderControlState::openTemp(TempTab t) {
    if (std::find(temps.begin(), temps.end(), t) == temps.end()) temps.push_back(t);
    shownTab = 1 + static_cast<int>(std::find(temps.begin(), temps.end(), t) - temps.begin());
    notify();
}

void DecoderControlState::closeTemp(TempTab t) {
    temps.erase(std::remove(temps.begin(), temps.end(), t), temps.end());
    shownTab = 0;
    notify();
}

}  // namespace bambi::ui
