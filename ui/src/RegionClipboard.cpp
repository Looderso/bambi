// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/ui/RegionClipboard.h"

namespace bambi::ui {
namespace {
constexpr juce::uint32 kLookEveryMs = 500;

struct Looked {
    bool valid{false};
    juce::uint32 at{0};
    std::optional<RegionClip> clip;
};
Looked& looked() {
    static Looked l;
    return l;
}
}  // namespace

TextClipboard& textClipboard() {
    static TextClipboard c{[] { return juce::SystemClipboard::getTextFromClipboard(); },
                           [](const juce::String& text) { juce::SystemClipboard::copyTextToClipboard(text); }};
    return c;
}

std::optional<RegionClip> regionOnClipboard() {
    auto& l = looked();
    const auto now = juce::Time::getMillisecondCounter();
    if (l.valid && now - l.at < kLookEveryMs) return l.clip;
    l.valid = true;
    l.at = now;
    const auto& read = textClipboard().read;
    l.clip = read ? readRegionClip(read().toStdString()) : std::nullopt;
    return l.clip;
}

void clipboardChanged() { looked().valid = false; }

void copyToClipboard(const RegionClip& clip) {
    if (const auto& write = textClipboard().write) write(juce::String(writeRegionClip(clip)));
    looked().valid = false;
}

}  // namespace bambi::ui
