// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/live/arrival.hpp"

#include <algorithm>
#include <cmath>

namespace bambi::live {

std::int64_t Arrivals::at(std::int64_t blockStart, double secondsAgo) noexcept {
    const auto ago = static_cast<std::int64_t>(std::llround(std::max(0.0, secondsAgo) * rate_));
    last_ = std::max(last_, blockStart - ago + offset_);
    return last_;
}

}  // namespace bambi::live
