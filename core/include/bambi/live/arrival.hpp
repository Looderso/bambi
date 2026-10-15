// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <limits>

/*  When a message from Live arrived, in the host's samples: the `at` the TransportProjector takes. The socket
 *  stamps every line with a steady clock as it arrives; the audio thread, which alone counts the host's samples,
 *  turns the stamp into a sample when it takes the message at the start of a block -- the block's first sample less
 *  how long ago the line came, plus `offsetSamples`, by which Live's audio for a position reaches the host after
 *  Live reports that position.
 *
 *  Never earlier than the last sample it gave: the projector takes messages in the order of their samples. A stamp
 *  from after the block's start counts as arriving at it. Arithmetic only, for the audio thread.
 */
namespace bambi::live {

class Arrivals {
public:
    explicit Arrivals(double rate, std::int64_t offsetSamples = 0) noexcept : rate_(rate), offset_(offsetSamples) {}

    std::int64_t at(std::int64_t blockStart, double secondsAgo) noexcept;

private:
    double rate_;
    std::int64_t offset_;
    std::int64_t last_{std::numeric_limits<std::int64_t>::min()};
};

}  // namespace bambi::live
