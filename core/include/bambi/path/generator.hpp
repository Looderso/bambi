// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <span>
#include <string_view>

#include "bambi/math/vec3.hpp"
#include "bambi/path/shape.hpp"

namespace bambi {

/*  Parametric trajectories, sampled analytically. genParams are in degrees where they are angles
 *  (they are user-facing authoring state); everything downstream is radians.
 */

/// Position at u in [0,1]. For a closed generator u wraps; for an open one it spans the path.
Vec3 generatorAt(GeneratorType g, double u, std::span<const double> genParams);

/// Whether the generator's path closes on itself.
bool generatorIsClosed(GeneratorType g);

/// How many sharp corners the path has, evenly spaced in u from u = 0; 1 for a path without.
int generatorCorners(GeneratorType g, std::span<const double> genParams);

/*  A generator's settings: the name the state saves each under, a lowercase label, "deg" or no unit,
 *  range, step and default. The one statement of them: the encoder's path parameters read their ranges
 *  and defaults from here. */
struct GeneratorParam {
    std::string_view name;
    std::string_view label;
    std::string_view unit;
    double min{0.0};
    double max{1.0};
    double step{1.0};
    double def{0.0};
    bool wraps{false};  ///< one turn -- an azimuth, a phase, a heading -- whose ends are one value
};

inline constexpr std::array<GeneratorParam, 3> kOrbitParams{{{"tilt", "tilt", "deg", 0, 90, 1, 30},
                                                             {"spin", "axis az", "deg", 0, 360, 1, 0, true},
                                                             {"aperture", "aperture", "deg", 5, 175, 1, 90}}};
inline constexpr std::array<GeneratorParam, 5> kLissajousParams{{{"az_amount", "az amount", "deg", 5, 180, 1, 80},
                                                                 {"el_amount", "el amount", "deg", 5, 90, 1, 45},
                                                                 {"az_ratio", "az ratio", "", 1, 7, 1, 1},
                                                                 {"el_ratio", "el ratio", "", 1, 7, 1, 2},
                                                                 {"phase", "phase", "deg", 0, 360, 1, 0, true}}};
inline constexpr std::array<GeneratorParam, 4> kWaveParams{{{"turns", "turns", "", 1, 4, 1, 1},
                                                            {"el_amount", "el amount", "deg", 0, 85, 1, 35},
                                                            {"wobbles", "wobbles", "", 1, 9, 1, 3},
                                                            {"el_offset", "el offset", "deg", -60, 60, 1, 0}}};
/// A regular polygon around the front, its corners `radius` from it: one side is a point at the front, two a line there and back.
inline constexpr std::array<GeneratorParam, 3> kPolygonParams{{{"sides", "sides", "", 1, 8, 1, 3},
                                                               {"radius", "radius", "deg", 1, 90, 1, 45},
                                                               {"rotation", "rotation", "deg", 0, 360, 1, 0, true}}};
inline constexpr std::array<GeneratorParam, 4> kSpiralParams{{{"turns", "turns", "", 1, 6, 1, 2},
                                                              {"from_el", "from el", "deg", -89, 89, 1, 80},
                                                              {"to_el", "to el", "deg", -89, 89, 1, -80},
                                                              {"start_az", "start az", "deg", 0, 360, 1, 0, true}}};

constexpr std::span<const GeneratorParam> generatorParams(GeneratorType g) {
    switch (g) {
        case GeneratorType::Orbit: return kOrbitParams;
        case GeneratorType::Lissajous: return kLissajousParams;
        case GeneratorType::Wave: return kWaveParams;
        case GeneratorType::Polygon: return kPolygonParams;
        case GeneratorType::Spiral: return kSpiralParams;
    }
    return {};
}

/// Setting `index` of shape `g`. In a constant expression an index out of range fails to compile.
constexpr const GeneratorParam& generatorParam(GeneratorType g, int index) {
    return generatorParams(g)[static_cast<std::size_t>(index)];
}

/// Each setting's default, written into genParams (kMaxGenParams wide); the rest are zero.
void generatorDefaults(GeneratorType g, std::span<double> genParams);

}  // namespace bambi
