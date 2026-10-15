// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

/*  bambi's own loop device, as Core Audio has it: whether it is installed, and the names of its channels, which it
 *  takes through its custom property 'bnam' and gives back as any device's channel names. Elsewhere than macOS
 *  there is no such device, and every answer says so.
 */
namespace bambi::loop {

/// Whether the device is installed and running, by the uid its build gives it.
bool installed();

/// The loop device an aggregate device holds -- the one a program playing through that aggregate reaches: "bambi",
/// else "BlackHole 16ch"; empty when no aggregate holds either.
std::string inAggregate();

/// Names its channels, channel 1 first. False when the device is not there or refuses.
bool nameChannels(const std::vector<std::string>& names);

/// A channel's name as any program reads it; empty when there is none to read.
std::string channelName(int channel);

}  // namespace bambi::loop
