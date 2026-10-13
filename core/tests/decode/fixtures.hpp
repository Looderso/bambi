// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "bambi/decode/allrad.hpp"
#include "bambi/decode/headmodelfile.hpp"
#include "bambi/decode/layoutfile.hpp"
#include "bambi/patch/json.hpp"
#include "doctest.h"

/*  What the decode tests share: the shipped head model, the array on hand, and the small helpers around them. The
 *  files are read from the source tree (BAMBI_CORE_DIR), which is why this lives under tests/ and never in the
 *  library. */
namespace bambi::test {

inline std::string readAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "cannot read " << path);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

/// core/data/decode/head.json: nobody's head and the template, learned from HUTUBS and 3D3A.
inline const HeadModel& shipped() {
    static const HeadModel model = [] {
        HeadModel m;
        std::string error;
        REQUIRE_MESSAGE(parseHeadModel(readAll(std::string(BAMBI_CORE_DIR) + "/data/decode/head.json"), m, &error),
                        error);
        return m;
    }();
    return model;
}

/// The array's IEM file as parsed JSON: its layout and the plug-in's own order-5 matrix.
inline const Json& arrayFile() {
    static const Json f = Json::parse(readAll(std::string(BAMBI_CORE_DIR) + "/tests/decode/array16.json"));
    return f;
}

/// The array as the room has it: the file marks channel 18, straight below, real; the room has nothing there.
inline Layout array16() {
    Layout l;
    std::string error;
    REQUIRE_MESSAGE(parseIemLayout(readAll(std::string(BAMBI_CORE_DIR) + "/tests/decode/array16.json"), l, &error),
                    error);
    for (Loudspeaker& s : l.speakers)
        if (s.channel == 18) s.imaginary = true;
    return l;
}

inline SpeakerDesign design(const Layout& l, int order, Weighting w = Weighting::Basic) {
    SpeakerDesign d;
    std::string error;
    REQUIRE_MESSAGE(designAllrad(l, order, w, d, &error), error);
    return d;
}

/// A unit vector from degrees.
inline Vec3 direction(double azimuthDeg, double elevationDeg) {
    return fromAzEl(azimuthDeg * kDeg2Rad, elevationDeg * kDeg2Rad);
}

/// Uniform noise in -1..1, `channels` x `frames` interleaved, the same for the same seed.
inline std::vector<float> noise(int channels, int frames, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    std::vector<float> x(static_cast<std::size_t>(channels * frames));
    for (float& v : x) v = u(rng);
    return x;
}

}  // namespace bambi::test
