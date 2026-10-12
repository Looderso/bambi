// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/live/latency.hpp"

#include <algorithm>

namespace bambi::live {

Compensation compensate(const HostLatency& latency) {
    const auto slowest = [](const std::vector<int>& paths) {
        int most = 0;
        for (const int p : paths) most = std::max(most, p);
        return most;
    };
    const int sources = slowest(latency.sources);
    const int sends = slowest(latency.sends);

    Compensation c;
    for (const int p : latency.sources) c.sourceDelay.push_back(sources - std::max(p, 0));
    c.dryDelay = sends;
    for (const int p : latency.sends) c.returnDelay.push_back(sends - std::max(p, 0));
    c.total = sources + sends + std::max(latency.main, 0);
    return c;
}

}  // namespace bambi::live
