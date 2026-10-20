// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <juce_core/juce_core.h>

#include "bambi/patch/identity.hpp"

namespace bambi::ui {

/// What a plugin is called on screen, lower-case as the header and the live host write it. Not its id in a
/// saved state, which `name(Product)` gives and which never changes.
inline juce::String productName(Product p) {
    switch (p) {
        case Product::Encoder: return "path encoder";
        case Product::Echo: return "echo";
        case Product::Reverb: return "reverb";
        case Product::Decoder: return "decoder";
        case Product::Unknown: return {};
    }
    return {};
}

/// Every plugin, for what is sized to the widest of their names.
inline constexpr Product kProducts[] = {Product::Encoder, Product::Echo, Product::Reverb, Product::Decoder};

}  // namespace bambi::ui
