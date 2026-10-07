// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>

#include "bambi/decode/headmodel.hpp"

namespace bambi {

/// Reads a head model from the text of its JSON file. False, with `error` set, when the text is not a version-1
/// head model with peak-shaped notches or its tables do not fit together; `out` is then unspecified.
bool parseHeadModel(std::string_view json, HeadModel& out, std::string* error = nullptr);

}  // namespace bambi
