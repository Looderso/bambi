// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <juce_graphics/juce_graphics.h>
#include <vector>

#include "bambi/scene/view.hpp"

/// How a line on the sphere is stroked: screen segments become two paths, near and far (fainter), each stroked once.
namespace bambi::ui {

/// `segments` may hold several lines end to end. `skipped` is scratch the caller keeps; `dashed` draws another instance's line.
void strokeSegments(juce::Graphics& g, const std::vector<ScreenSegment>& segments,
                    std::vector<juce::Point<double>>& skipped, juce::Colour colour, float width, bool dashed);

/// How many strokes this has handed the platform since the process began. For checks.
int strokesIssued();

}  // namespace bambi::ui
