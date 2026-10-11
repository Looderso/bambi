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

/*  A decoder as filters: a SOFA GeneralFIR-E file with one measurement, the field's channels as its emitters and
 *  the loudspeaker feeds as its receivers -- what tools/matrix/jconvolver-to-sofa.py writes from a jconvolver set.
 *  Its global attributes say the order, channel order and normalisation the filters expect on their input. */
struct FirMatrix {
    double sampleRate{48000.0};
    int inputs{0}, outputs{0}, taps{0};
    std::vector<float> filters;  ///< inputs x outputs x taps
    int order{-1};               ///< AmbisonicsOrder, or -1 when the file does not say
    std::string channelOrder;    ///< AmbisonicsChannelOrder: "ACN" expected
    std::string normalization;   ///< AmbisonicsNormalization: "N3D" or "SN3D"; empty when the file does not say
    std::string title;
    std::span<const float> filter(int input, int output) const {
        return {filters.data() + (static_cast<std::size_t>(input) * static_cast<std::size_t>(outputs) +
                                  static_cast<std::size_t>(output)) *
                                     static_cast<std::size_t>(taps),
                static_cast<std::size_t>(taps)};
    }
};
/// False, with `error` set, when the file does not open, is not FIR-E, or has more than one measurement. Allocates.
bool readSofaMatrix(const std::string& path, FirMatrix& out, std::string* error = nullptr);

}  // namespace bambi
