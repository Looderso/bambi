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

/// The most a device's stamp is believed to lie before the message's arrival, in milliseconds. Older is a clock that
/// stepped or a stamp of another time, and the arrival stands.
inline constexpr double kStampTrustMs = 5000.0;

/*  When the device read Live's position, on the socket's steady clock. `readMs` is the device's stamp on the system's
 *  wall clock; `steadyNowNs` and `systemNowMs` are the two clocks read together, now. What happened to the message
 *  after the read -- the device's queue, the relay, the socket -- then no longer moves where Live was. Without a
 *  stamp, or with one after the arrival or older than kStampTrustMs, the arrival `arrivedNs` stands. */
std::int64_t readAtNs(std::int64_t arrivedNs, std::int64_t steadyNowNs, double systemNowMs, double readMs) noexcept;

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
