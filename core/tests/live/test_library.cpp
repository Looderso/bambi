// SPDX-License-Identifier: GPL-3.0-or-later
//
//  Where Live keeps its User Library, from its preferences folders and Library.cfg.

#include <string>
#include <vector>

#include "bambi/live/library.hpp"
#include "doctest.h"

using bambi::live::newestLiveFolder;
using bambi::live::userLibraryFromConfig;

TEST_CASE("the newest Live version's folder is found by its numbers, not by its name's order") {
    //  Catches: the folders compared as text, which takes "Live 12.9" over "Live 12.10", or the first or last one
    //  listed taken.
    std::vector<std::string> folders{"Live 12.10", "Live 12.9.3", "Live 11.3.21", "Live 12.4.6"};
    CHECK(newestLiveFolder(folders) == "Live 12.10");
}

TEST_CASE("a patch release is newer than the release it patches") {
    //  Catches: only the first two numbers compared, which ties "Live 12.4" and "Live 12.4.6" and keeps the one
    //  seen first.
    std::vector<std::string> folders{"Live 12.4", "Live 12.4.6", "Live 12.3.8"};
    CHECK(newestLiveFolder(folders) == "Live 12.4.6");
}

TEST_CASE("a release is newer than its own beta, whichever is listed first") {
    //  Catches: the beta's suffix ignored, which ties "Live 12.2b5" with "Live 12.2" and leaves the pick to the
    //  order the folders are listed in.
    std::vector<std::string> betaFirst{"Live 12.2b5", "Live 12.2"};
    std::vector<std::string> releaseFirst{"Live 12.2", "Live 12.2b5"};
    CHECK(newestLiveFolder(betaFirst) == "Live 12.2");
    CHECK(newestLiveFolder(releaseFirst) == "Live 12.2");
}

TEST_CASE("an absurdly long version number is read without overflowing") {
    //  Catches: every digit accumulated into an int, which overflows (undefined behaviour) on a long run.
    std::vector<std::string> folders{"Live 99999999999999999999", "Live 12.4"};
    CHECK(newestLiveFolder(folders) == "Live 99999999999999999999");
}

TEST_CASE("folders that are not a Live version's are passed over") {
    //  Catches: any folder name taken when it parses to nothing, or "Live" with no number taken as a version.
    std::vector<std::string> folders{"Live", "Max 9", ".DS_Store", "Live 12.1"};
    CHECK(newestLiveFolder(folders) == "Live 12.1");
    std::vector<std::string> none{"Live", "Max 9"};
    CHECK(newestLiveFolder(none).empty());
}

TEST_CASE("the User Library is the folder Library.cfg names, with User Library appended") {
    //  Catches: the ProjectPath of another entry taken, or the folder returned without its User Library.
    const char* cfg = R"(<?xml version="1.0" encoding="UTF-8"?>
<Ableton>
	<ContentLibrary>
		<ProjectPath Value="/Users/someone/Music/Factory" />
	</ContentLibrary>
	<UserLibrary>
		<LibraryProject>
				<ProjectName Value="User Library" />
				<ProjectPath Value="/Volumes/Audio/Ableton" />
		</LibraryProject>
	</UserLibrary>
</Ableton>)";
    CHECK(userLibraryFromConfig(cfg) == "/Volumes/Audio/Ableton/User Library");
}

TEST_CASE("a library under another name is found by the name Library.cfg gives it") {
    //  Catches: "User Library" appended whatever the entry names, which misses a renamed library.
    CHECK(userLibraryFromConfig(R"(<UserLibrary><LibraryProject><ProjectName Value="Mine" />
<ProjectPath Value="/Users/a/Music" /></LibraryProject></UserLibrary>)") == "/Users/a/Music/Mine");
}

TEST_CASE("a path with an escaped character or a trailing slash comes back as the folder's real path") {
    //  Catches: the entity left in the path, which names a folder that does not exist, or a doubled slash.
    CHECK(userLibraryFromConfig(R"(<UserLibrary><ProjectPath Value="/Users/a/Rock &amp; Roll/" /></UserLibrary>)") ==
          "/Users/a/Rock & Roll/User Library");
}

TEST_CASE("a Library.cfg without a User Library entry names none") {
    //  Catches: a ProjectPath outside the UserLibrary entry taken, which would put the device in another library.
    CHECK(userLibraryFromConfig(R"(<ContentLibrary><ProjectPath Value="/x" /></ContentLibrary>)").empty());
    CHECK(userLibraryFromConfig(R"(<UserLibrary></UserLibrary><Other><ProjectPath Value="/x" /></Other>)").empty());
    CHECK(userLibraryFromConfig(R"(<UserLibrary><ProjectPath Value="" /></UserLibrary>)").empty());
}
