// SPDX-License-Identifier: GPL-3.0-or-later
#include "LiveDevice.h"

#include <juce_core/juce_core.h>
#include <string>
#include <vector>

#include "HostLog.h"
#include "bambi/live/library.hpp"

namespace bambi::livehost {

namespace {

/// The User Library of the newest Live whose preferences name one, else Live's default place; an empty File when
/// that folder does not exist, which means no Live to install for.
juce::File userLibrary() {
    const auto home = juce::File::getSpecialLocation(juce::File::userHomeDirectory);
    const auto prefs = home.getChildFile("Library/Preferences/Ableton");
    std::vector<std::string> folders;
    for (const auto& f : prefs.findChildFiles(juce::File::findDirectories, false))
        folders.push_back(f.getFileName().toStdString());
    if (const auto newest = live::newestLiveFolder(folders); !newest.empty()) {
        const auto cfg = prefs.getChildFile(newest).getChildFile("Library.cfg");
        if (const auto path = live::userLibraryFromConfig(cfg.loadFileAsString().toStdString()); !path.empty()) {
            const juce::File named(path);
            return named.isDirectory() ? named : juce::File();
        }
    }
    const auto fallback = home.getChildFile("Music/Ableton/User Library");
    return fallback.isDirectory() ? fallback : juce::File();
}

}  // namespace

void installLiveDevice() {
    const auto source = juce::File::getSpecialLocation(juce::File::currentApplicationFile)
                            .getChildFile("Contents/Resources/device");
    if (!source.isDirectory()) return;
    const auto library = userLibrary();
    if (library == juce::File()) {
        hostLog("the Live device: no User Library found, not installed");
        return;
    }
    const auto target = library.getChildFile("Presets/Audio Effects/Max Audio Effect/bambi");
    int copied = 0;
    for (const auto& file : source.findChildFiles(juce::File::findFiles, false)) {
        const auto installed = target.getChildFile(file.getFileName());
        if (installed.existsAsFile() && installed.hasIdenticalContentTo(file)) continue;
        if (!target.createDirectory() || !file.copyFileTo(installed)) {
            hostLog("the Live device: could not write " + installed.getFullPathName().toStdString());
            return;
        }
        ++copied;
    }
    if (copied > 0)
        hostLog("the Live device: " + std::to_string(copied) + " file(s) copied to " +
                target.getFullPathName().toStdString());
}

}  // namespace bambi::livehost
