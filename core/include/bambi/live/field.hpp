// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "bambi/live/graph.hpp"
#include "bambi/live/pairs.hpp"
#include "bambi/live/protocol.hpp"
#include "bambi/patch/json.hpp"

/*  Which of a Live set's tracks and groups the field takes, and as what: the user's shape laid over the set as it
 *  is now.
 *
 *  A node at the top of the set that plays to Main with audio is in the field once the shape takes it -- until then
 *  it plays in Live, as every node does in a shape that takes none; a return never. Such a node is a source -- its stereo, whatever Live made of it, arrives on a pair of its own and is encoded -- unless it stays
 *  not taken, or is a group the user split: then it is a bus of the field, and each of its children that plays into
 *  it with audio is laid out in turn, a source or, a group split again, a bus inside it. A split group none of whose
 *  children reaches the field is not in it. The shape's marks on a node not where they apply are ignored: "taken"
 *  below the top, "split" on a node that is no group.
 *
 *  Each part of the field, and Main, may carry a chain of inserts, Echo or Reverb, each with an id of its own that
 *  stays with it as the chain changes around it, so its instance and its settings do. A chain on a node not in the
 *  field is kept in the shape and plays nowhere. A source's Encoder, and an insert, may take a sidechain from a
 *  source of the field -- a source never from itself: that track's
 *  own stereo, as it arrives on its pair, before its Encoder; a sidechain whose track is not a source with a pair
 *  hears silence, and is kept for when it is one again.
 *
 *  A Live return the shape takes into the field is a return of the field: its chain plays what is wired to it.
 *  Live's own return is then to be silent, or it would be heard as well.
 *
 *  Where each part, return and Main plays is a list of wires, each into a bus of the field, a return of the field,
 *  Main, or one Decoder's own mix, at a level of its own, after the node's inserts. A node the shape gives no wires
 *  plays as Live would: a part into its bus, or Main; a return into Main; Main into every Decoder -- each at 0 dB. A
 *  Decoder hears the sum of what is wired to it, Main's chain included only through Main's wire. What may go
 *  where: a part anywhere but into itself; a return into Main or a Decoder; Main into a Decoder; never round a loop
 *  of buses. A wire the field cannot play -- to a bus, a return or a Decoder it does not have, or one that is not
 *  allowed -- is left out of the field and kept in the shape, for when it can.
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

/*  An input of one of the host's devices played as a source of the field -- a microphone on stage -- at the top,
 *  into Main, beside Live's tracks; with an Encoder, inserts and a sidechain as any source. The set keeps it, with
 *  its chain; which device channel it is heard from is the computer's, since another has other devices. */
struct HostInput {
    TrackId id{0};  ///< kFirstInput and up: above any id Live gives, and exact as JSON's numbers are
    std::string name;
    int width{1};  ///< 1, or 2 for a stereo pair
    friend bool operator==(const HostInput&, const HostInput&) = default;
};
inline constexpr TrackId kFirstInput = TrackId{1} << 40;
inline constexpr std::uint64_t kFirstDecoder = 1;  ///< the Decoder a set that names none has
inline bool isInput(TrackId id) { return id >= kFirstInput; }

/// Where a wire plays into: a bus or a return of the field, by its node; Main; or a Decoder's own mix, by its id.
struct Target {
    enum class Kind : std::uint8_t { Node, Main, Decoder };
    Kind kind{Kind::Main};
    std::uint64_t id{0};  ///< the bus's or the return's track, the Decoder's id; 0 for Main

    static Target node(TrackId n) { return {Kind::Node, n}; }
    static Target main() { return {Kind::Main, 0}; }
    static Target decoder(std::uint64_t d) { return {Kind::Decoder, d}; }
    friend bool operator==(const Target&, const Target&) = default;
};

inline constexpr double kSilentDb = -60.0;  ///< a wire at or below this level is silent
inline constexpr double kLoudestDb = 6.0;   ///< the highest level a wire takes
/// A wire's level as a gain: 0 at or below kSilentDb, the level clamped to kLoudestDb above.
float gainOf(double db);

struct Wire {
    Target to;
    double db{0.0};  ///< kSilentDb to kLoudestDb
    friend bool operator==(const Wire&, const Wire&) = default;
};

/// A node's wires as the user set them -- none at all, too: it plays nowhere. kMain for Main's.
struct Routing {
    TrackId node{kMain};
    std::vector<Wire> wires;
    friend bool operator==(const Routing&, const Routing&) = default;
};

struct Shape {
    std::vector<TrackId> split;         ///< groups whose children the field takes one by one, the group their bus
    std::vector<TrackId> taken;         ///< top-level nodes the field takes; every other plays in Live
    std::vector<Chain> chains;          ///< one per node with inserts, Main's among them
    std::vector<TrackId> returns;       ///< Live's returns the field plays, its own
    std::vector<Sidechain> sidechains;  ///< the sources whose Encoder's sidechain hears another track
    std::vector<HostInput> inputs;      ///< the host's own sources, after Live's in the field
    /// The Decoders, by ids of their own, each its own instance hearing Main; none: one, kFirstDecoder
    std::vector<std::uint64_t> decoders;
    std::vector<Routing> routing;  ///< the nodes whose wires are not their default, Main's among them
    friend bool operator==(const Shape&, const Shape&) = default;

    /// The inserts on `node`, kMain for Main's; none when it has no chain.
    const std::vector<Insert>& insertsOn(TrackId node) const;
    /// The track `source`'s Encoder's sidechain hears; 0 none.
    TrackId sidechainOf(TrackId source) const;
};

struct Part {
    TrackId node{0};
    bool bus{false};  ///< a split group; otherwise a source, encoded from its pair
    int parent{-1};   ///< the bus Live has it in, by index into the field's parts; -1: none, at the top
    std::vector<Insert> inserts;
    TrackId sidechain{0};     ///< a source's: the track its Encoder's sidechain hears; 0 none
    std::vector<Wire> wires;  ///< where it plays, each a wire the field can play
    friend bool operator==(const Part&, const Part&) = default;
};

struct FieldReturn {
    TrackId node{0};  ///< the Live return
    std::vector<Insert> inserts;
    std::vector<Wire> wires;
    friend bool operator==(const FieldReturn&, const FieldReturn&) = default;
};

struct Field {
    std::vector<Part> parts;              ///< a bus before everything in it
    std::vector<FieldReturn> returns;     ///< in Live's order
    std::vector<Insert> main;             ///< Main's chain, before the decoders
    std::vector<std::uint64_t> decoders;  ///< at least one
    std::vector<Wire> mainWires;          ///< Main's, each into a Decoder
    friend bool operator==(const Field&, const Field&) = default;

    /// The sources, in the order of the parts: what needs a pair.
    std::vector<TrackId> sources() const;
    /// The wires of `node` -- a part's, a return's, kMain for Main's; none for a node not in the field.
    const std::vector<Wire>& wiresOf(TrackId node) const;
};

Field layOut(const Tree& tree, const Shape& shape);

/// Every node at the top of `tree` that plays to Main with audio taken into the field, besides those already.
void takeAll(Shape& shape, const Tree& tree);

/// Whether `from` -- a part, a return, kMain -- may be wired to `to` in `field` as it is: what it goes to is in
/// the field, the kinds allow it, and it closes no loop.
bool canWire(const Field& field, TrackId from, const Target& to);

/// What a Decoder's own mix is made of: every node wired into it, at its level, and whether that node reaches
/// the Decoder another way too -- through Main, a bus, a return -- so that it is heard twice.
struct Heard {
    TrackId node{kMain};
    double db{0.0};
    bool twice{false};
    friend bool operator==(const Heard&, const Heard&) = default;
};
std::vector<Heard> heardBy(const Field& field, std::uint64_t decoder);

/// A wire from `from` set: put, or its level changed when there is one to that target already. A node with no
/// wires of its own in the shape takes the ones it plays now in `field` first, so its others stay. False, and the
/// shape as it was, when the field cannot play it -- `canWire`.
bool setWire(Shape& shape, const Field& field, TrackId from, const Wire& wire);
/// The wire from `from` to `to` taken out, the same way; the node may be left playing nowhere.
void removeWire(Shape& shape, const Field& field, TrackId from, const Target& to);

/// Where a source arrives on the input row (graph.hpp): its first channel and its width.
struct Arrival {
    TrackId track{0};
    int channel{0};
    int width{2};
    friend bool operator==(const Arrival&, const Arrival&) = default;
};
/// Live's tracks as they arrive: each on its pair of the loop device, after Live's Main's.
std::vector<Arrival> arrivalsOf(const std::vector<TrackPair>& pairs);

/// The plan that plays a field: a node per source that arrives, encoded by an Encoder slot of its own, slots
/// 0, 1, ... in the order of the parts; a node per bus; a return per return of the field; then a slot per
/// insert, the parts' in their order, the returns' and Main's last; then a slot per decoder, in the field's order. A source
/// that does not arrive -- a track past the loop device's pairs, an input whose device is not chosen -- is left out
/// with its inserts, and a bus keeps its place. An insert's slot names where its sidechain's source arrives.
/// The slots' latencies are the caller's to fill.
struct FieldPlan {
    Plan plan;
    std::vector<TrackId> encoders;        ///< the track each Encoder slot encodes
    std::vector<Insert> effects;          ///< the insert each slot after the Encoders plays
    std::vector<std::uint64_t> decoders;  ///< the decoder each slot after the inserts plays
};
FieldPlan planOf(const Field& field, const std::vector<Arrival>& arrivals);

/// Every wire's level as a gain, by the runner's entries (graph.hpp's `wiresOf`): its own level, as `field` has it
/// now -- so a level changed in the shape needs no new plan -- times Live's fader
/// where Live has one the host's audio comes after -- a bus at its group's, silent while the group is muted, since
/// its children left Live before both; a return at its own, not its mute, which the device sets in Live so that only
/// the field plays it. A source plays at its own level alone: Live has applied its fader before the pair.
std::vector<float> levels(const FieldPlan& plan, const Field& field, const Tree& tree);

/// The shape as the set keeps it, and back; anything unreadable is left out.
Json toJson(const Shape& shape);
Shape shapeFrom(const Json& json);

}  // namespace bambi::live
