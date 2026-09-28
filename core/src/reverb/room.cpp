// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/reverb/room.hpp"

#include <algorithm>
#include <cmath>

namespace bambi {
namespace {

constexpr double kAirDecay = 10.0;            // s: the highs' decay if the air were the only loss
constexpr double kToneHigh[2] = {0.35, 1.0};  // the highs' decay as a share of the mids', at tone 0 and 1
constexpr double kToneLow[2] = {1.3, 1.0};    // and the lows'
constexpr double kDriest = 0.95;              // a room cannot absorb more than this at a bounce

double lerp(double a, double b, double t) { return a + (b - a) * t; }

}  // namespace

Proportions proportionsOf(RoomShape shape) {
    switch (shape) {
        case RoomShape::Room: return {1.0, 1.25, 0.65};
        case RoomShape::Hall: return {1.0, 1.8, 0.6};
        case RoomShape::Tall: return {1.0, 1.1, 1.2};
    }
    return {1.0, 1.25, 0.65};
}

double eyringAbsorption(double volume, double surface, double rt60) {
    return 1.0 - std::exp(-0.161 * volume / (surface * rt60));
}

double eyringDecay(double volume, double surface, double absorption) {
    return 0.161 * volume / (-surface * std::log(1.0 - absorption));
}

Room deriveRoom(const RoomSettings& s) {
    const Proportions p = proportionsOf(s.shape);
    const double size = std::max(s.size, 0.5), tone = std::clamp(s.tone, 0.0, 1.0);
    Room r;
    r.x = size * p.depth;
    r.y = size * p.width;
    r.z = size * p.height;
    r.volume = r.x * r.y * r.z;
    r.surface = 2.0 * (r.x * r.y + r.x * r.z + r.y * r.z);

    r.minDecay = eyringDecay(r.volume, r.surface, kDriest);
    r.tooDry = s.decay < r.minDecay - 1e-6;
    r.rtMid = std::max(s.decay, r.minDecay);
    const double highMaterial = r.rtMid * lerp(kToneHigh[0], kToneHigh[1], tone);
    r.rtLow = r.rtMid * lerp(kToneLow[0], kToneLow[1], tone);
    r.rtHigh = 1.0 / (1.0 / highMaterial + 1.0 / kAirDecay);  // the walls and the air, together
    r.absorbMid = eyringAbsorption(r.volume, r.surface, r.rtMid);
    r.absorbHigh = eyringAbsorption(r.volume, r.surface, highMaterial);
    r.absorbLow = eyringAbsorption(r.volume, r.surface, r.rtLow);

    //  sqrt(V) milliseconds, the reflection-density predictor, held between 25 -- a small room keeps its
    //  first reflections -- and 80, the early/late boundary listeners hear.
    r.mixingTime = std::clamp(std::sqrt(r.volume), 25.0, 80.0) / 1000.0;
    //  Small rooms get shorter lines, but not below 0.55: shorter loops sound metallic.
    r.lineScale = std::clamp(1.2 * std::cbrt(r.volume / 8640.0), 0.55, 2.0);
    const double rough = std::clamp(s.roughness, 0.0, 1.0);
    r.scatter = 0.8 * rough;
    r.diffusion = rough;
    return r;
}

}  // namespace bambi
