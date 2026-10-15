// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "bambi/live/pairs.hpp"

/*  The host's one shape, as a plan and as the steps that play it. The plan says what the user built: sources, each a
 *  track encoded where it arrives; buses, the groups in the field, each summing what plays into it; the inserts on
 *  each; returns in bambi mode, which every node in the field sends to; the main chain; the one decoder. compile()
 *  turns it into a list of steps a runner plays block by block, with every delay that makes the paths meet in step
 *  -- which is where the plan is tested to the sample, before any plugin is loaded.
 *
 *  A plugin instance is a slot, by its index; the runner owns the instances and reports each one's latency into
 *  the plan. A send's level is no part of the plan: it changes every block, so a step names the entry of the
 *  runner's table it reads, node * returns + return.
 *
 *  Paths meet in three kinds of place -- a bus, a return, Main -- and at each every input is delayed to the slowest
 *  of them, the slowest by nothing. A send is taken after its node's inserts. A node's steps come after all its
 *  children's, the returns' after every node's, Main's last. Allocates; for the message thread, never the audio's.
 */
namespace bambi::live {

struct Slot {
    int latency{0};     ///< samples, as the plugin reports it; a negative report counts as 0
    int sidechain{-1};  ///< an Encoder's or an insert's: the loop device's pair its sidechain hears; -1 silence
};

struct FieldNode {
    TrackId track{0};
    int pair{-1};     ///< a source: the loop device's pair it arrives on. -1: a bus, the sum of what plays into it
    int encoder{-1};  ///< a source's encoder slot; a bus has none
    int parent{-1};   ///< the bus it plays into, by index; -1: Main
    std::vector<int> inserts;
    friend bool operator==(const FieldNode&, const FieldNode&) = default;
};

struct Return {
    std::vector<int> chain;  ///< slots
};

struct Plan {
    std::vector<Slot> slots;
    std::vector<FieldNode> nodes;
    std::vector<Return> returns;
    std::vector<int> main;  ///< the main chain's slots, before the decoder
    int decoder{-1};
};

enum class Step {
    Encode,   ///< slot reads pair `from` of the loop device and writes buffer `to`
    Process,  ///< slot runs on buffer `to`, in place
    Clear,    ///< buffer `to` silent
    Add,      ///< buffer `to` += buffer `from`, delayed by `delay` samples, times the level `gain` names (-1: unity)
    Decode,   ///< slot reads buffer `from` and writes the output
};

struct Op {
    Step step{Step::Clear};
    int slot{-1};
    int from{-1};
    int to{-1};
    int delay{0};
    int gain{-1};
    int sidechain{-1};  ///< Encode, Process: the loop device's pair the slot's sidechain hears; -1 silence
    friend bool operator==(const Op&, const Op&) = default;
};

struct Compiled {
    std::vector<Op> ops;
    int buffers{0};  ///< field buffers: one per node, then one per return, then Main's
    int latency{0};  ///< samples from the loop device to the output, the same on every path
    int mainBuffer() const { return buffers - 1; }
};

/// The steps that play `plan`, or none with `error` set when the plan does not hold together: a source without a
/// pair or an encoder, a bus with an encoder, a parent that is no bus or does not exist, a cycle, a slot used twice
/// or out of range, no decoder.
std::optional<Compiled> compile(const Plan& plan, std::string* error = nullptr);

}  // namespace bambi::live
