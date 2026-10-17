// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

#include "bambi/live/io.hpp"

/*  The system's audio devices as Core Audio has them, and the private aggregate the live host plays through: bambi's
 *  loop device first, so the host's input and output rows begin with Live's channels, then the chosen devices in
 *  order. The first chosen is the clock; every other device, the loop device among them, is drift corrected to it.
 *  The aggregate is private -- seen by this program alone -- and goes when its owner does. Elsewhere than macOS there
 *  are no devices and no aggregate.
 *
 *  Message thread: Core Audio's device list may block while a device comes or goes.
 */
namespace bambi::devices {

/// The uid of bambi's loop device.
inline constexpr const char* kLoopUid = "bambi_UID";

/// Every device but the loop device and aggregates, with its channels and latencies in samples at its rate now.
std::vector<live::AudioDevice> system();

/// The loop device's channels, each way; 0 when it is not there.
int loopChannels();

/// A private aggregate of the loop device and `devices`, the first the clock, while this lives.
class Aggregate {
public:
    Aggregate() = default;
    /// False, with `error`, when Core Audio would not make it or the loop device is not there.
    bool create(const std::vector<std::string>& devices, std::string* error = nullptr);
    void destroy();
    ~Aggregate() { destroy(); }
    Aggregate(const Aggregate&) = delete;
    Aggregate& operator=(const Aggregate&) = delete;

    bool exists() const { return id_ != 0; }
    /// The name a program opens it by.
    static constexpr const char* kName = "bambi live host";

private:
    unsigned id_{0};
};

}  // namespace bambi::devices
