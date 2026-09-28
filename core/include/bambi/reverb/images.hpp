// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>

#include "bambi/math/vec3.hpp"
#include "bambi/reverb/room.hpp"

/*  Reverb's early reflections as geometry: the image sources of the box, and the twelve virtual
 *  sources the bus is read at. A source mirrored in the walls is a source behind them: along each
 *  axis, index n mirrors it n times, giving a reflection order |nx| + |ny| + |nz| (6 images at order
 *  1, 24 at order 2, 62 at order 3). Each image arrives later and quieter by its extra path and a
 *  wall's loss per bounce, and duller; of its energy, (1 - scatter)^order stays a mirror image and the
 *  rest scatters into the tail at the same delay (spec^2 + scat^2 is the whole energy), fading past
 *  the mixing time since nothing is heard as a discrete reflection by then. Pure geometry: no audio,
 *  no allocation, everything in metres and seconds.
 */
namespace bambi {

inline constexpr int kReflectionOrder = 3;
inline constexpr int kMaxImages = 62;
inline constexpr int kVirtualSources = 12;
inline constexpr double kSpeedOfSound = 343.0;
inline constexpr double kSpreadSeconds = 0.003;  ///< copies of one reflection land within this of their average

struct ImageIndex {
    int nx, ny, nz, order;
};

/// The images up to an order, in a fixed sequence (x outermost) so image i is the same reflection for every source. Returns the count written.
int imageIndices(int order, std::array<ImageIndex, kMaxImages>& out);

struct Reflection {
    Vec3 direction{1.0, 0.0, 0.0};  ///< where it arrives from
    double delay{0.0};              ///< seconds after the source itself
    double distance{0.0};           ///< metres, image to listener
    double gain{0.0};               ///< pressure, against the source at 1: path and walls
    double mirror{0.0};             ///< the part heard as a reflection, from its direction
    double scattered{0.0};          ///< the part the walls scattered or the mixing time took: into the tail
    double cutoffHz{20000.0};       ///< how dull: the walls' extra loss of highs a bounce, and the air along the path
    int order{0};
};

/// A source's distance in a direction, pulled in to stay inside the room: no nearer a wall than a tenth of the way.
double sourceRadius(const Room& room, Vec3 direction, double distance);

/// The reflections of a source in `direction` at `distance` (pulled inside the room), into `out`. Returns imageIndices(kReflectionOrder).
int reflectionsOf(const Room& room, Vec3 direction, double distance, std::array<Reflection, kMaxImages>& out);

/// The virtual sources' directions: evenly spread, a golden-angle spiral.
std::array<Vec3, kVirtualSources> virtualSourceDirections();

/// What a direction contributes when the field is read or placed at an order with a max-rE beam (1 along the beam, falling away from it); `cosine` is of the angle between them.
double maxReBeam(int order, double cosine);

/// The bus's reflections: every virtual source, at the assumed distance in its own direction, with its
/// own images. Neighbouring sources give the same image a few milliseconds apart, which would smear
/// or comb if summed directly, so each copy keeps its own offset, scaled to stay within kSpreadSeconds.
struct BusReflections {
    std::array<Vec3, kVirtualSources> direction{};
    std::array<std::array<Reflection, kMaxImages>, kVirtualSources> taps{};
    int count{0};  ///< reflections a virtual source
};
void busReflections(const Room& room, double distance, BusReflections& out);

}  // namespace bambi
