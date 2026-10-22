// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/*  The Max for Live device the host talks to travels inside the host's bundle (Contents/Resources/device). At start
 *  the host copies it into Live's User Library, Presets/Audio Effects/Max Audio Effect/bambi, where Live's browser
 *  lists it: a file that is missing or differs from the bundle's is copied, so a new host brings its own device. No
 *  User Library, no copy. Message thread; logs what it did.
 */
namespace bambi::livehost {

void installLiveDevice();

}  // namespace bambi::livehost
