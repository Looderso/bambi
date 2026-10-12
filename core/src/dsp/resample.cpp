// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/dsp/resample.hpp"

#include <cmath>

namespace bambi {

void resampleSinc(std::span<const float> in, double fromHz, double toHz, std::vector<float>& out, int halfWidth) {
    out.clear();
    if (in.empty() || !(fromHz > 0) || !(toHz > 0) || halfWidth < 1) return;
    const double step = fromHz / toHz;                    // input samples per output sample
    const double cutoff = step > 1.0 ? 1.0 / step : 1.0;  // of the input's Nyquist: the lower of the two
    const auto count = static_cast<std::size_t>(std::llround(static_cast<double>(in.size()) / step));
    out.resize(count);
    const double pi = 3.14159265358979323846;
    for (std::size_t k = 0; k < count; ++k) {
        const double t = static_cast<double>(k) * step;
        const auto centre = static_cast<long>(std::floor(t));
        double acc = 0.0;
        for (long j = centre - halfWidth + 1; j <= centre + halfWidth; ++j) {
            if (j < 0 || j >= static_cast<long>(in.size())) continue;
            const double x = t - static_cast<double>(j);
            const double sinc = x == 0.0 ? 1.0 : std::sin(pi * cutoff * x) / (pi * cutoff * x);
            const double window = 0.5 + 0.5 * std::cos(pi * x / halfWidth);
            acc += static_cast<double>(in[static_cast<std::size_t>(j)]) * sinc * window;
        }
        out[k] = static_cast<float>(acc * cutoff);
    }
}

}  // namespace bambi
