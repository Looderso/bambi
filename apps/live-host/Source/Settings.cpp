// SPDX-License-Identifier: GPL-3.0-or-later
#include "Settings.h"

namespace bambi::livehost {

juce::File audioSettingsFile() {
    auto data = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
#if JUCE_MAC
    data = data.getChildFile("Application Support");  // JUCE's answer on a Mac is ~/Library
#endif
    return data.getChildFile("bambi").getChildFile("live-host.json");
}

live::AudioSettings loadAudioSettings() {
    const juce::File file = audioSettingsFile();
    if (!file.existsAsFile()) {
        live::AudioSettings fresh;
        fresh.copies.push_back({live::Signal::Headphones, {}, true});
        return fresh;
    }
    return live::audioSettingsFrom(Json::parse(file.loadFileAsString().toStdString()));
}

bool saveAudioSettings(const live::AudioSettings& settings) {
    const juce::File file = audioSettingsFile();
    return file.getParentDirectory().createDirectory().wasOk() &&
           file.replaceWithText(juce::String(toJson(settings).dump(2)));
}

}  // namespace bambi::livehost
