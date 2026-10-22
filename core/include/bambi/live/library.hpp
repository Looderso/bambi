// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <span>
#include <string>
#include <string_view>

/*  Where Live keeps its User Library, read from Live's own preferences, so the host can put the Max for Live device
 *  where Live's browser lists it. Live writes one preferences folder per version ("Live 12.4.6") under
 *  ~/Library/Preferences/Ableton; the newest one's Library.cfg names the folder that holds the User Library. Pure
 *  text: the host reads the folders and the file.
 */
namespace bambi::live {

/// The name among `folders` that is the newest Live version's preferences folder ("Live 12.4.6" beats
/// "Live 12.4", "Live 12.10" beats "Live 12.9", "Live 12.2" beats "Live 12.2b5"); empty when none is a Live
/// version's.
std::string newestLiveFolder(std::span<const std::string> folders);

/// The User Library's path from the text of a Library.cfg: the folder its UserLibrary entry names, joined with the
/// library's name there ("User Library" when it gives none). Empty when the text names no folder.
std::string userLibraryFromConfig(std::string_view libraryCfg);

}  // namespace bambi::live
