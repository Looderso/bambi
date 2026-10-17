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

/*  Where a source arrives is a run of channels on the host's input row: the loop device's channels -- a track's
 *  pair -- then every chosen device's inputs. Channels on it are counted from 0. */
struct Slot {
    int latency{0};     ///< samples, as the plugin reports it; a negative report counts as 0
    int sidechain{-1};  ///< an Encoder's or an insert's: the input row's first channel its sidechain hears; -1 silence
    int sidechainWidth{2};  ///< 2, or 1 for a mono input, heard on both sides
};

struct FieldNode {
    TrackId track{0};
    int input{-1};  ///< a source: the input row's first channel it arrives on. -1: a bus, the sum of what plays into it
    int width{2};   ///< a source's channels there: 2, or 1 for a mono input, heard on both sides
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
    Encode,   ///< slot reads `width` channels of the input row from `from` and writes buffer `to`
    Process,  ///< slot runs on buffer `to`, in place
    Clear,    ///< buffer `to` silent
    Add,      ///< buffer `to` += buffer `from`, delayed by `delay` samples, times the level `gain` names
    Decode,   ///< slot reads buffer `from` and writes the output
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
    int buffers{0};  ///< field buffers: one per node, then one per return, then Main's
    int latency{0};  ///< samples from the input row to the output, the same on every path
    /// Entries in the runner's level table: a send's, node * returns + return; then each node's where it plays
    /// into its bus or Main, nodes * returns + node; then each return's into Main, nodes * returns + nodes + return.
    int levels{0};
    int mainBuffer() const { return buffers - 1; }
};

/// The steps that play `plan`, or none with `error` set when the plan does not hold together: a source without an
/// input or an encoder, a source's or a sidechain's width but 1 or 2, a bus with an encoder, a parent that is no bus or does not exist, a cycle, a slot used twice
/// or out of range, no decoder.
std::optional<Compiled> compile(const Plan& plan, std::string* error = nullptr);

}  // namespace bambi::live
