// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdio>

#include <juce_core/juce_core.h>

namespace bambi::host {

/// A picture tool's `--scale N`, anywhere after the output file: pixels per window point, 2 by default.
/// The flag and its value are removed from argv, so the options after it parse as if it were absent.
/// A value that is not above 0 keeps the default.
inline float takeShotScale(int& argc, char** argv) {
    for (int i = 2; i + 1 < argc; ++i) {
        if (juce::String(argv[i]) != "--scale") continue;
        const float scale = juce::String(argv[i + 1]).getFloatValue();
        for (int j = i; j + 2 < argc; ++j) argv[j] = argv[j + 2];
        argc -= 2;
        if (scale > 0.0f) return scale;
        std::fprintf(stderr, "--scale needs a number above 0; drawing at 2\n");
        break;
    }
    return 2.0f;
}

}  // namespace bambi::host
