// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/*  Reverb's one room: everything the engine needs, derived from the six things a user sets. Balance
 *  of reflections and tail, absorption, scattering, diffusion, damping, the mixing time and the
 *  lines' lengths are not controls -- set by hand they sound off -- so they are worked out here, in
 *  one place, from the room.
 *
 *  Metres and seconds. x is front, so a room's depth lies along x, its width along y, its height z.
 */
namespace bambi {

enum class RoomShape { Room, Hall, Tall };

struct RoomSettings {
    double size{20.0};  ///< the box's width, metres
    RoomShape shape{RoomShape::Hall};
    double decay{1.9};      ///< RT60 at mid frequencies, seconds
    double tone{0.5};       ///< 0..1: how much faster the highs die and how much longer the lows ring
    double roughness{0.5};  ///< 0..1: smooth walls mirror, rough ones scatter
};

struct Room {
    double x{0}, y{0}, z{0};  ///< depth, width, height
    double volume{0}, surface{0};
    double minDecay{0};                    ///< the driest this box can be: Eyring at an absorption of 0.95
    bool tooDry{false};                    ///< the decay asked for was shorter than that, and was raised to it
    double rtMid{0}, rtLow{0}, rtHigh{0};  ///< seconds; rtHigh includes the air's own loss
    double absorbMid{0}, absorbLow{0}, absorbHigh{0};  ///< per bounce; absorbHigh is the walls' alone
    double mixingTime{0};                              ///< seconds: where discrete reflections give way to the tail
    double lineScale{0};                               ///< the tail's 100..200 ms lines are this many times as long
    double scatter{0};                                 ///< the share of a reflection that scatters at each bounce
    double diffusion{0};
};

/// width : depth : height of a shape -- room 1 : 1.25 : 0.65, hall 1 : 1.8 : 0.6, tall 1 : 1.1 : 1.2.
struct Proportions {
    double width, depth, height;
};
Proportions proportionsOf(RoomShape shape);

/// Eyring, both ways: the absorption that gives a decay in a room, and the decay an absorption gives.
double eyringAbsorption(double volume, double surface, double rt60);
double eyringDecay(double volume, double surface, double absorption);

Room deriveRoom(const RoomSettings& settings);

}  // namespace bambi
