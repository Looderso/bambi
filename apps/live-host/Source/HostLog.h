// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <juce_core/juce_core.h>
#include <mutex>
#include <string>

/*  What the host did about its connection, one line each with the time, in ~/Library/Logs/bambi-live-host.log: listening or why not,
 *  a device that came, what it said, and that it went. A saved set whose device does not reach the host leaves nothing else to
 *  look at. Kept small: a file past 256 KB starts again when the host starts. Any thread.
 */
namespace bambi::livehost {

inline void hostLog(const std::string& line) {
    static std::mutex mutex;
    static bool started = false;
    const std::lock_guard<std::mutex> lock(mutex);
    const auto file = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                          .getChildFile("Library/Logs/bambi-live-host.log");
    if (!started) {
        started = true;
        if (file.getSize() > 256 * 1024) file.deleteFile();
        file.appendText("\n");
    }
    file.appendText(juce::Time::getCurrentTime().formatted("%Y-%m-%d %H:%M:%S  ") + juce::String(line) + "\n");
}

}  // namespace bambi::livehost
