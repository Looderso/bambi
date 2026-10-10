// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <span>
#include <string>

#include "bambi/decode/measuredhead.hpp"

/*  A SOFA file (AES69) as a measured head: every measurement's two receivers are the two ears, the
 *  first the left as the convention puts it, the source positions the directions. Read through
 *  libmysofa, whose check refuses a file that breaks the convention.
 */
namespace bambi {

/// False, with `error` set, when the file does not open, is not a SOFA file of impulse responses, or has not
/// two receivers. Allocates.
bool readSofa(const std::string& path, MeasuredHead& out, std::string* error = nullptr);
/// The same from the file's bytes.
bool readSofaData(std::span<const char> bytes, MeasuredHead& out, std::string* error = nullptr);

}  // namespace bambi
