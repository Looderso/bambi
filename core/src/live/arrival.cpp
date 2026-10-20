// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/live/arrival.hpp"

#include <algorithm>
#include <cmath>

namespace bambi::live {

std::int64_t readAtNs(std::int64_t arrivedNs, std::int64_t steadyNowNs, double systemNowMs, double readMs) noexcept {
    if (!(readMs > 0.0)) return arrivedNs;
    const auto read = steadyNowNs - static_cast<std::int64_t>(std::llround((systemNowMs - readMs) * 1e6));
    const bool believable = read <= arrivedNs && static_cast<double>(arrivedNs - read) <= kStampTrustMs * 1e6;
    return believable ? read : arrivedNs;
}

std::int64_t Arrivals::at(std::int64_t blockStart, double secondsAgo) noexcept {
    const auto ago = static_cast<std::int64_t>(std::llround(std::max(0.0, secondsAgo) * rate_));
    last_ = std::max(last_, blockStart - ago + offset_);
    return last_;
}

}  // namespace bambi::live
