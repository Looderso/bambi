// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bambi/live/field.hpp"
#include "bambi/live/pairs.hpp"
#include "bambi/live/protocol.hpp"

/*  The host's side of the conversation with the bambi Live device: a message in, the answers out, and what it knows
 *  of the set meanwhile. Written once for every program that stands where the host stands -- the host, and the
 *  stand-in that proves the device without it -- which add only the socket.
 *
 *  A hello is welcomed. A tree is the set as it is now: every track, group or return the device sent, and the
 *  answer is the pairs -- one for every source of the field the user's shape lays over it (field.hpp), each
 *  keeping the pair it had; then the session's state to keep with the set, and what the host says about the mix.
 *  A new shape is laid over the last tree, and answered the same way.
 *
 *  Grouping tracks never changes what is heard: a group in a new tree that holds, anywhere below it, a track the
 *  field played as a source is split, so its tracks stay sources with their Encoders and inserts, and the group
 *  becomes their bus, the group at the top taken as they were. Only a tree from Live does this, never the user's own shape. A restore
 *  gives back the state an earlier session kept. A change of a value is taken into the tree.
 *
 *  What the set keeps for the host is the pairs, the shape and the parts the host gives the session by name -- its plugins'
 *  states -- each sent again whenever the host asks, and all of it given back by a restore.
 *
 *  The loop device's channels, named for the pairs: "<track> L" and "<track> R" on a pair's two, the binaural
 *  return on the pair past the last.
 *
 *  Allocates; for the message thread.
 */
namespace bambi::live {

struct SessionSettings {
    int pairCount{31};      ///< the loop device's pairs for tracks, after Live's Main's on its first two channels
    bool loop{true};        ///< the loop device is there
    double latencyMs{0.0};  ///< the spatial mix behind a dry track, as the host's plan has it
    double rate{48000.0};
};

class DeviceSession {
public:
    explicit DeviceSession(SessionSettings settings = {}) : s_(settings) {}

    /// What to send back, in order; nothing for most messages.
    std::vector<Message> answer(const Message& in);

    /// The user's shape: the routing it makes, and the state with it, when a tree has come; nothing before.
    std::vector<Message> reshape(Shape shape);
    const Shape& shape() const { return shape_; }
    /// The field the shape lays over the last tree.
    const Field& field() const { return field_; }

    void setLoop(bool present) { s_.loop = present; }
    void setLatency(double ms, double rate) {
        s_.latencyMs = ms;
        s_.rate = rate;
    }

    bool connected() const { return hello_.has_value(); }
    const std::optional<Hello>& hello() const { return hello_; }
    /// The device speaks this host's protocol -- or has not said, and is taken to. A device that does not is welcomed once, in
    /// the host's, and its other messages are not answered or acted on.
    bool compatible() const { return !hello_.has_value() || accepts(*hello_); }
    bool on() const { return on_; }
    const Tree& tree() const { return tree_; }
    /// Whether a tree has come: before it, tree() says nothing of the set, not that it is empty.
    bool hasTree() const { return treed_; }
    const std::vector<TrackPair>& pairs() const { return pairs_; }
    const std::vector<TrackId>& unassigned() const { return unassigned_; }
    const Routed& routed() const { return routed_; }
    const std::optional<TransportMessage>& transport() const { return transport_; }
    int shows() const { return shows_; }  ///< times "open host" was pressed

    /// A part of what the set keeps, beside the pairs, replacing the one of that name. One named "pairs" or
    /// "shape" is never saved: those are the session's own.
    void keep(std::string_view name, Json value);
    /// The part of that name, as kept or restored; none when there is none.
    const Json* kept(std::string_view name) const { return kept_.find(name); }
    /// Live's arrangement as the device last sent it: empty before it has.
    const Arrangement& arrangement() const { return arrangement_; }
    /// What the set should keep now.
    Save save() const { return Save{state()}; }

    /// The node on a pair, as the tree has it now; none when no track is on it.
    const Node* trackOn(int pair) const;

    /// The loop device's channel names, channel 1 first: 2 x (pairCount + 1) of them -- Main's, then the tracks' --
    /// unnamed ones empty.
    std::vector<std::string> channelNames() const;

    /// The device went away: what it said is forgotten, the pairs are kept for when it comes back.
    void disconnected();

private:
    std::vector<Message> onTree(const Tree& t, bool adopt);
    Json state() const;

    SessionSettings s_;
    std::optional<Hello> hello_;
    bool on_{false};
    Tree tree_;
    bool treed_{false};  ///< a tree has come
    Shape shape_;
    Field field_;
    std::vector<TrackPair> pairs_;
    std::vector<TrackId> unassigned_;
    Json kept_ = Json::object();
    Arrangement arrangement_;
    Routed routed_;
    std::optional<TransportMessage> transport_;
    int shows_{0};
};

}  // namespace bambi::live
