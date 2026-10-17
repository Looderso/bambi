// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <vector>

#include "bambi/live/graph.hpp"
#include "bambi/live/pairs.hpp"
#include "bambi/live/protocol.hpp"
#include "bambi/patch/json.hpp"

/*  Which of a Live set's tracks and groups the field takes, and as what: the user's shape laid over the set as it
 *  is now.
 *
 *  Every node at the top of the set that plays to Main with audio is in the field, a return never. Such a node is a
 *  source -- its stereo, whatever Live made of it, arrives on a pair of its own and is encoded -- unless it stays
 *  in Live, or is a group the user split: then it is a bus of the field, and each of its children that plays into
 *  it with audio is laid out in turn, a source or, a group split again, a bus inside it. A split group none of whose
 *  children reaches the field is not in it. The shape's marks on a node not where they apply are ignored: "in Live"
 *  below the top, "split" on a node that is no group.
 *
 *  Each part of the field, and Main, may carry a chain of inserts, Echo or Reverb, each with an id of its own that
 *  stays with it as the chain changes around it, so its instance and its settings do. A chain on a node not in the
 *  field is kept in the shape and plays nowhere. A source's Encoder, and an insert, may take a sidechain from a
 *  source of the field -- a source never from itself: that track's
 *  own stereo, as it arrives on its pair, before its Encoder; a sidechain whose track is not a source with a pair
 *  hears silence, and is kept for when it is one again.
 *
 *  A Live return the shape takes into the field is a return of the field: every part sends to it, after its
 *  inserts, at the level its own send knob for that return has in Live, and its chain plays what they send. Live's
 *  own return is then to be silent, or it would be heard as well.
 *
 *  Allocates; for the message thread.
 */
namespace bambi::live {

enum class Effect { Echo, Reverb };

struct Insert {
    std::uint64_t id{0};  ///< > 0, the shape's own, never reused for another insert
    Effect effect{Effect::Echo};
    TrackId sidechain{0};  ///< the track whose stereo its sidechain hears, a source of the field; 0 none
    friend bool operator==(const Insert&, const Insert&) = default;
};

inline constexpr TrackId kMain = 0;  ///< the chain on Main; no track has this id

struct Chain {
    TrackId node{kMain};
    std::vector<Insert> inserts;  ///< in the order they play
    friend bool operator==(const Chain&, const Chain&) = default;
};

struct Sidechain {
    TrackId source{0};  ///< a track whose Encoder takes it
    TrackId from{0};    ///< the track its sidechain hears
    friend bool operator==(const Sidechain&, const Sidechain&) = default;
};

struct Shape {
    std::vector<TrackId> split;         ///< groups whose children the field takes one by one, the group their bus
    std::vector<TrackId> inLive;        ///< top-level nodes that stay in Live: nothing of them reaches the field
    std::vector<Chain> chains;          ///< one per node with inserts, Main's among them
    std::vector<TrackId> returns;       ///< Live's returns the field plays, its own
    std::vector<Sidechain> sidechains;  ///< the sources whose Encoder's sidechain hears another track
    friend bool operator==(const Shape&, const Shape&) = default;

    /// The inserts on `node`, kMain for Main's; none when it has no chain.
    const std::vector<Insert>& insertsOn(TrackId node) const;
    /// The track `source`'s Encoder's sidechain hears; 0 none.
    TrackId sidechainOf(TrackId source) const;
};

struct Part {
    TrackId node{0};
    bool bus{false};  ///< a split group; otherwise a source, encoded from its pair
    int parent{-1};   ///< the bus it plays into, by index into the field's parts; -1: Main
    std::vector<Insert> inserts;
    TrackId sidechain{0};  ///< a source's: the track its Encoder's sidechain hears; 0 none
    friend bool operator==(const Part&, const Part&) = default;
};

struct FieldReturn {
    TrackId node{0};  ///< the Live return
    int send{0};      ///< its place among Live's returns: which of a track's send knobs feeds it
    std::vector<Insert> inserts;
    friend bool operator==(const FieldReturn&, const FieldReturn&) = default;
};

struct Field {
    std::vector<Part> parts;           ///< a bus before everything in it
    std::vector<FieldReturn> returns;  ///< in Live's order
    std::vector<Insert> main;          ///< Main's chain, before the decoder
    friend bool operator==(const Field&, const Field&) = default;

    /// The sources, in the order of the parts: what needs a pair.
    std::vector<TrackId> sources() const;
};

Field layOut(const Tree& tree, const Shape& shape);

/// The plan that plays a field: a node per source with a pair, encoded by an Encoder slot of its own, slots
/// 0, 1, ... in the order of the parts; a node per bus; a return per return of the field; then a slot per
/// insert, the parts' in their order, the returns' and Main's last; the decoder the slot after them all. A source without a pair -- one past the loop device's pairs --
/// is left out with its inserts, and a bus keeps its place. An insert's slot names the pair its sidechain hears.
/// The slots' latencies are the caller's to fill.
struct FieldPlan {
    Plan plan;
    std::vector<TrackId> encoders;  ///< the track each Encoder slot encodes
    std::vector<Insert> effects;    ///< the insert each slot after the Encoders plays
};
FieldPlan planOf(const Field& field, const std::vector<TrackPair>& pairs);

/// Every level the plan plays at, as gains, by the runner's entries (graph.hpp's `Compiled::levels`):
///   - a part's send to a return: its node's send knob for that return in `tree`, in dB -- times the fader, for a
///     bus, since Live's sends follow it; a knob the tree does not have, or one at silence, is 0;
///   - a part where it plays into its bus or Main: a source at unity -- Live has applied its fader before the pair
///     -- a bus at its group's fader, and silent while the group is muted, since its children left Live before both;
///   - a return of the field where it plays into Main: its fader in Live. Not its mute: the device mutes it in Live,
///     so that only the field plays it.
std::vector<float> levels(const FieldPlan& plan, const Field& field, const Tree& tree);

/// The shape as the set keeps it, and back; anything unreadable is left out.
Json toJson(const Shape& shape);
Shape shapeFrom(const Json& json);

}  // namespace bambi::live
