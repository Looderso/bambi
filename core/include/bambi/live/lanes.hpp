// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "bambi/live/protocol.hpp"
#include "bambi/patch/json.hpp"

/*  The live host's automation: a lane drives one parameter of one instance the host plays, against Live's song
 *  position. The instance is named as the set names it -- an Encoder by its track, an insert or a Decoder by its
 *  id -- and the parameter by its stable key, so a lane outlives the chain changing around it, and a value is kept
 *  in the parameter's own units, not as a position on today's range.
 *
 *  Between two points the value runs straight; before the first it holds the first, after the last the last. A
 *  lane without points drives nothing. Pure; for the message thread, which hands the audio thread its own copy.
 */
namespace bambi::live {

enum class Instance { Encoder, Insert, Decoder };

struct LanePoint {
    double ppq{0.0};    ///< quarter notes from the song's start
    double value{0.0};  ///< in the parameter's own units
    friend bool operator==(const LanePoint&, const LanePoint&) = default;
};

struct Lane {
    std::uint64_t id{0};  ///< > 0, the lane's own, never reused
    Instance instance{Instance::Encoder};
    std::uint64_t owner{0};         ///< the Encoder's track, the insert's id, the Decoder's id
    std::string param;              ///< the parameter's stable key
    std::vector<LanePoint> points;  ///< in time order, no two at one time
    /// A clip lane: the track whose session clip it follows, and that clip's slot; its points' times are then in
    /// the clip, from its start, and it loops with it. A song lane: none, -1.
    TrackId clipTrack{0};
    int slot{-1};
    std::string clipName;  ///< the clip's name, to find it again when the scenes move
    bool clip() const { return slot >= 0; }
    friend bool operator==(const Lane&, const Lane&) = default;
};

/// The value `points` give at `ppq`; none without points.
std::optional<double> valueAt(const std::vector<LanePoint>& points, double ppq);

/// A point put at its time, replacing one already there. Returns its index.
std::size_t putPoint(Lane& lane, LanePoint p);
/// Point `index` moved to `to`, its time held between its neighbours'. Returns its index.
std::size_t movePoint(Lane& lane, std::size_t index, LanePoint to);
void removePoint(Lane& lane, std::size_t index);

/// The point within `reach` quarter notes and `valueReach` of the value at `ppq`, nearest in time; none else.
std::optional<std::size_t> pointNear(const Lane& lane, double ppq, double value, double reach, double valueReach);

/// Where a session clip is `elapsed` quarter notes after Live said it was at `at`: on from there, back to the loop's
/// start at its end while it loops, held at its end when it does not.
double clipPosition(const SlotPlay& at, double elapsed);

/// Every clip lane onto its clip again, as the session is now: a lane whose slot holds another clip moves to the one
/// slot on its track that holds a clip of its clip's name -- the scenes were moved -- and a lane whose slot still
/// holds a clip and whose name is nowhere else takes that clip's name -- it was renamed in place. True when any
/// lane changed.
bool followClips(std::vector<Lane>& lanes, const Arrangement& a);

/// The lanes as the set keeps them, and back; a lane unreadable is left out.
Json toJson(const std::vector<Lane>& lanes);
std::vector<Lane> lanesFrom(const Json& json);

}  // namespace bambi::live
