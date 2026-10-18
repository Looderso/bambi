// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "bambi/live/pairs.hpp"

/*  The host's one shape, as a plan and as the steps that play it. The plan says what the user built: sources, each a
 *  track encoded where it arrives; buses, each summing what is wired into it; the inserts on each; returns in bambi
 *  mode; the main chain; the decoders. Every node, return and Main plays along wires: a node into a bus, a return,
 *  Main or a decoder's own mix; a return into Main or a mix; Main into mixes. Each decoder hears its mix. compile()
 *  turns it into a list of steps a runner plays block by block, with every delay that makes the paths meet in step
 *  -- which is where the plan is tested to the sample, before any plugin is loaded.
 *
 *  A plugin instance is a slot, by its index; the runner owns the instances and reports each one's latency into
 *  the plan. A wire's level is no part of the plan: it changes every block, so a step names the entry of the
 *  runner's table it reads -- one per wire, the nodes' wires in their order, then the returns', then Main's.
 *
 *  Paths meet where things are summed -- a bus, a return, Main -- and at each every input is delayed to the slowest
 *  of them, the slowest by nothing. The decoders' mixes are all held to one time, the slowest input of any of them,
 *  so every listener, and Live's Main beside them, hears the same moment. A wire is taken after its node's inserts.
 *  Allocates; for the message thread, never the audio's.
 */
namespace bambi::live {

/*  Where a source arrives is a run of channels on the host's input row: the loop device's channels -- a track's
 *  pair -- then every chosen device's inputs. Channels on it are counted from 0. */
struct Slot {
    int latency{0};     ///< samples, as the plugin reports it; a negative report counts as 0
    int sidechain{-1};  ///< an Encoder's or an insert's: the input row's first channel its sidechain hears; -1 silence
    int sidechainWidth{2};  ///< 2, or 1 for a mono input, heard on both sides
};

/// Where a wire plays into, by index: a node of the plan -- a bus --, a return, Main, a decoder's mix.
struct PlanWire {
    enum class To : std::uint8_t { Node, Return, Main, Decoder };
    To to{To::Main};
    int index{0};  ///< the node, the return, the decoder by its place in the plan's; Main: unused
    friend bool operator==(const PlanWire&, const PlanWire&) = default;
};

struct FieldNode {
    TrackId track{0};
    int input{-1};  ///< a source: the input row's first channel it arrives on. -1: a bus, the sum of what plays into it
    int width{2};   ///< a source's channels there: 2, or 1 for a mono input, heard on both sides
    int encoder{-1};  ///< a source's encoder slot; a bus has none
    int parent{-1};   ///< the bus it plays into while `wires` is unset, by index; -1: Main
    std::vector<int> inserts;
    std::optional<std::vector<PlanWire>> wires;  ///< unset: into `parent` at its entry's level
    friend bool operator==(const FieldNode&, const FieldNode&) = default;
};

struct Return {
    std::vector<int> chain;                      ///< slots
    std::optional<std::vector<PlanWire>> wires;  ///< unset: into Main
    friend bool operator==(const Return&, const Return&) = default;
};

struct Plan {
    std::vector<Slot> slots;
    std::vector<FieldNode> nodes;
    std::vector<Return> returns;
    std::vector<int> main;                           ///< the main chain's slots, before the decoders
    std::vector<int> decoders;                       ///< one slot each; at least one
    std::optional<std::vector<PlanWire>> mainWires;  ///< unset: into every decoder's mix
};

enum class Step {
    Encode,   ///< slot reads `width` channels of the input row from `from` and writes buffer `to`
    Process,  ///< slot runs on buffer `to`, in place
    Clear,    ///< buffer `to` silent
    Add,      ///< buffer `to` += buffer `from`, delayed by `delay` samples, times the level `gain` names
    Decode,   ///< slot reads buffer `from`, its mix, and writes decoder `to`'s output, by its place in the plan's
};

struct Op {
    Step step{Step::Clear};
    int slot{-1};
    int from{-1};
    int to{-1};
    int delay{0};
    int gain{-1};
    int sidechain{-1};      ///< Encode, Process: the input row's first channel the slot's sidechain hears; -1 silence
    int width{2};           ///< Encode: the source's channels
    int sidechainWidth{2};  ///< Encode, Process: the sidechain's channels
    friend bool operator==(const Op&, const Op&) = default;
};

struct Compiled {
    std::vector<Op> ops;
    int buffers{0};  ///< field buffers: one per node, then one per return, then Main's, then each decoder's mix
    int decoders{0};
    /// Samples from the input row to the output, the same on every path through the slowest decoder; a quicker
    /// one's output leads by the difference, which whoever plays them out makes up.
    int latency{0};
    int levels{0};  ///< entries in the runner's level table: one per wire
    int mainBuffer() const { return buffers - decoders - 1; }
};

/// The wires the plan plays, a default filled in where it left them unset, in the order of the level table's
/// entries: each with where it comes from -- a node, a return, Main -- by index.
struct WireOf {
    enum class From : std::uint8_t { Node, Return, Main };
    From from{From::Node};
    int index{0};
    PlanWire wire;
    friend bool operator==(const WireOf&, const WireOf&) = default;
};
std::vector<WireOf> wiresOf(const Plan& plan);

/// The steps that play `plan`, or none with `error` set when the plan does not hold together: a source without an
/// input or an encoder, a source's or a sidechain's width but 1 or 2, a bus with an encoder, a parent that is no bus
/// or does not exist, a wire to something that is not there or that the kinds do not allow, a node wired into
/// itself, a cycle, a slot used twice or out of range, no decoder.
std::optional<Compiled> compile(const Plan& plan, std::string* error = nullptr);

}  // namespace bambi::live
