// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "bambi/live/pairs.hpp"
#include "bambi/live/protocol.hpp"

/*  The host's side of the conversation with the bambi Live device: a message in, the answers out, and what it knows
 *  of the set meanwhile. Written once for every program that stands where the host stands -- the host, and the
 *  stand-in that proves the device without it -- which add only the socket.
 *
 *  A hello is welcomed. A tree is the set as it is now: every track, group or return the device sent, and the
 *  answer is the pairs -- one for every node at the top of the set that plays to Main and has audio, a return
 *  never, a node inside a group never (its group carries it), a track playing elsewhere never -- each keeping the
 *  pair it had; then the session's state to keep with the set, and what the host says about the mix. A restore
 *  gives back the state an earlier session kept. A change of a value is taken into the tree.
 *
 *  The loop device's channels, named for the pairs: "<track> L" and "<track> R" on a pair's two, the binaural
 *  return on the pair past the last.
 *
 *  Allocates; for the message thread.
 */
namespace bambi::live {

struct SessionSettings {
    int pairCount{7};       ///< the loop device's pairs for tracks; the one past them carries the binaural return
    bool loop{true};        ///< the loop device is there
    double latencyMs{0.0};  ///< the spatial mix behind a dry track, as the host's plan has it
    double rate{48000.0};
};

class DeviceSession {
public:
    explicit DeviceSession(SessionSettings settings = {}) : s_(settings) {}

    /// What to send back, in order; nothing for most messages.
    std::vector<Message> answer(const Message& in);

    void setLoop(bool present) { s_.loop = present; }
    void setLatency(double ms, double rate) {
        s_.latencyMs = ms;
        s_.rate = rate;
    }

    bool connected() const { return hello_.has_value(); }
    const std::optional<Hello>& hello() const { return hello_; }
    bool on() const { return on_; }
    const Tree& tree() const { return tree_; }
    const std::vector<TrackPair>& pairs() const { return pairs_; }
    const std::vector<TrackId>& unassigned() const { return unassigned_; }
    const Routed& routed() const { return routed_; }
    const std::optional<TransportMessage>& transport() const { return transport_; }
    int shows() const { return shows_; }  ///< times "open host" was pressed

    /// The loop device's channel names, channel 1 first: 2 x (pairCount + 1) of them, unnamed ones empty.
    std::vector<std::string> channelNames() const;

    /// The device went away: what it said is forgotten, the pairs are kept for when it comes back.
    void disconnected();

private:
    std::vector<Message> onTree(const Tree& t);
    Json state() const;

    SessionSettings s_;
    std::optional<Hello> hello_;
    bool on_{false};
    Tree tree_;
    std::vector<TrackPair> pairs_;
    std::vector<TrackId> unassigned_;
    Routed routed_;
    std::optional<TransportMessage> transport_;
    int shows_{0};
};

}  // namespace bambi::live
