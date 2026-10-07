// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>

#include "bambi/decode/layout.hpp"

namespace bambi {

/// Reads a layout from the text of an IEM AllRADecoder configuration (the format SPARTA reads and writes too):
/// LoudspeakerLayout.Loudspeakers[] with Azimuth, Elevation, Radius, IsImaginary, Channel, Gain. False, with
/// `error` set, when the text is not one; `out` is then unspecified.
bool parseIemLayout(std::string_view json, Layout& out, std::string* error = nullptr);

}  // namespace bambi
