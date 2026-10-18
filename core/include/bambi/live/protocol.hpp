// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "bambi/live/pairs.hpp"
#include "bambi/live/transport.hpp"
#include "bambi/patch/json.hpp"

/*  What the bambi Live device and the host say to each other: one JSON object per line, its kind in "type". Written
 *  once here for every program that speaks it -- the host, and the stand-in that proves the device without it. The
 *  device's side is a script in Max, which must say the same.
 *
 *  Reading is tolerant as state reading is: a field missing or of the wrong kind takes its default, and a field
 *  nobody knows is ignored, so either side can grow a field first. A "type" nobody knows is refused, as is a line
 *  that is not a JSON object. Whether a hello's protocol is one this side speaks is accepts()'s to say, not
 *  parse()'s: the answer to a hello in the wrong protocol is still a welcome, saying which this side speaks.
 *
 *  Levels are dB as Live shows them; silence is no value (JSON's null), JSON having no infinity. Allocates;
 *  for the message thread.
 */
namespace bambi::live {

inline constexpr int kProtocol = 1;

//  ---- the device to the host ------------------------------------------------------------------------------------

struct Hello {
    int protocol{kProtocol};
    std::string live;  ///< Live's version, "12.4"
    double rate{0.0};  ///< 0: the device cannot tell
    friend bool operator==(const Hello&, const Hello&) = default;
};

struct Switched {  ///< the route button: "bambi" on the wire
    bool on{false};
    friend bool operator==(const Switched&, const Switched&) = default;
};

struct Node {
    enum class Kind { Track, Group, Return };
    enum class Path { Main, Elsewhere };  ///< where its audio goes: towards Main, or nowhere bambi can follow
    TrackId id{0};
    Kind kind{Kind::Track};
    std::optional<TrackId> parent;  ///< the group it plays into; none when it plays to Main
    Path path{Path::Main};
    std::string name;
    std::string colour;                        ///< "#rrggbb"
    bool audio{true};                          ///< it has audio to give
    std::vector<std::optional<double>> sends;  ///< per return, dB
    std::optional<double> fader;               ///< dB
    bool mute{false};
    bool solo{false};
    double pan{0.0};  ///< -1 left .. 1 right
    friend bool operator==(const Node&, const Node&) = default;
};

struct Tree {
    std::vector<Node> nodes;
    friend bool operator==(const Tree&, const Tree&) = default;
};

/// One observed change. `value` is a number, a string or null as the change has it: "send:<n>", "fader" (dB),
/// "pan", "mute", "solo", "name", "colour", "routing".
struct Value {
    TrackId id{0};
    std::string name;
    std::string what;
    Json value;
};

struct Restore {
    Json state;
};

/*  Live's arrangement, as guidance for placing automation: every track's clips, read-only here; the locators; the
 *  loop. Sent when bambi turns on and again when a track's clips change -- never while it plays. Times in quarter
 *  notes from the song's start. */
struct Clip {
    double start{0.0}, end{0.0};
    std::string name;
    std::string colour;  ///< "#rrggbb"
    bool midi{false};
    friend bool operator==(const Clip&, const Clip&) = default;
};
struct TrackClips {
    TrackId id{0};
    std::vector<Clip> clips;  ///< in time order
    friend bool operator==(const TrackClips&, const TrackClips&) = default;
};
struct Locator {
    double time{0.0};
    std::string name;
    friend bool operator==(const Locator&, const Locator&) = default;
};
/*  A track's session clip as Live plays it: which slot, and where in the clip Live is, sent as Live reports it --
 *  when the track's playing slot changes and as the clip's position moves. In quarter notes from the clip's start;
 *  a looping clip's position runs from its loop's start to its end and back. */
struct SlotPlay {
    TrackId id{0};
    int slot{-1};          ///< the scene's index; -1 when no session clip plays on the track
    double position{0.0};  ///< where in the clip Live is
    double loopStart{0.0}, loopEnd{0.0};
    bool looping{false};
    /// The session has the track, not the arrangement: from a clip launched on it until Back to Arrangement,
    /// whether a clip still plays or not.
    bool overrides{false};
    friend bool operator==(const SlotPlay&, const SlotPlay&) = default;
};

/// A session clip in its slot: what the session view draws a clip lane against.
struct SessionClip {
    int slot{0};  ///< the scene's index
    std::string name;
    std::string colour;                   ///< "#rrggbb"
    double loopStart{0.0}, loopEnd{0.0};  ///< quarter notes in the clip; the whole clip when it does not loop
    bool looping{false};
    bool midi{false};
    friend bool operator==(const SessionClip&, const SessionClip&) = default;
};
struct TrackSlots {
    TrackId id{0};
    std::vector<SessionClip> clips;  ///< the slots that hold one, in scene order
    friend bool operator==(const TrackSlots&, const TrackSlots&) = default;
};

struct Arrangement {
    std::vector<TrackClips> tracks;
    std::vector<TrackSlots> session;  ///< every track's session clips, read with the arrangement
    int scenes{0};
    std::vector<Locator> locators;  ///< in time order
    bool loopOn{false};
    double loopStart{0.0}, loopLength{0.0};
    double readMs{0.0};  ///< how long reading it held Live up, as the device timed it
    friend bool operator==(const Arrangement&, const Arrangement&) = default;
};

struct Routed {
    struct Track {
        TrackId id{0};
        std::string name;
        bool ok{false};
        std::string to;     ///< where it went, when ok
        std::string error;  ///< why not, when not
        friend bool operator==(const Track&, const Track&) = default;
    };
    std::vector<Track> tracks;
    friend bool operator==(const Routed&, const Routed&) = default;
};

struct Show {  ///< "open host" pressed
    friend bool operator==(const Show&, const Show&) = default;
};

//  ---- the host to the device ------------------------------------------------------------------------------------

struct Welcome {
    int protocol{kProtocol};
    bool loop{true};  ///< the host found its loop device
    friend bool operator==(const Welcome&, const Welcome&) = default;
};

struct Route {
    std::vector<TrackPair> tracks;  ///< every track bambi plays and its pair; one not here goes back
    std::vector<TrackId> silent;    ///< Live's returns the field plays: silent in Live; one not here as it was
    friend bool operator==(const Route&, const Route&) = default;
};

struct Save {
    Json state;
};

struct Info {
    double latency{0.0};  ///< ms, the spatial mix behind a dry track
    double rate{0.0};
    std::map<TrackId, std::string> warnings;
    friend bool operator==(const Info&, const Info&) = default;
};

struct Status {
    std::string text;
    friend bool operator==(const Status&, const Status&) = default;
};

struct Busy {  ///< the host has another device
    friend bool operator==(const Busy&, const Busy&) = default;
};

struct CreateReturn {
    std::string name;
    friend bool operator==(const CreateReturn&, const CreateReturn&) = default;
};

/// The arrangement and the session's clips, read again and sent when they changed: the host asks while its timeline
/// is open, since Live does not report every change. The device reads no more often than fifty times the last
/// read's cost, so Live's own thread stays free while it plays.
struct Refresh {
    friend bool operator==(const Refresh&, const Refresh&) = default;
};

/// Live's transport, asked from the host: Live stays the clock, and reports back what it did as any transport.
struct Command {
    enum class Action { Play, Stop, Continue, Locate };
    Action action{Action::Play};
    double ppq{0.0};  ///< Locate's: where to, quarter notes from the song's start
    friend bool operator==(const Command&, const Command&) = default;
};

using Message = std::variant<Hello, Switched, Tree, Value, TransportMessage, Restore, Arrangement, SlotPlay, Routed,
                             Show, Welcome, Route, Save, Info, Status, Busy, CreateReturn, Command, Refresh>;

/// A line as a message, or none with `error` set when it is not a JSON object or its type is unknown.
std::optional<Message> parse(std::string_view line, std::string* error = nullptr);

/// The message as one line of JSON, without the newline.
std::string serialise(const Message& m);

/// Whether this side speaks the protocol a hello asks for.
inline bool accepts(const Hello& h) { return h.protocol == kProtocol; }

}  // namespace bambi::live
