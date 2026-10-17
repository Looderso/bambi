// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

#include "bambi/patch/state.hpp"

/*  Where the decoder's two outputs sit on the host's bus, from the state alone: the headphones' pair, and the
 *  layout's loudspeakers -- or a filter matrix's outputs -- moved as a block to start on the layout's first
 *  channel. Pure functions of the state, so a page reads them from whatever state it shows and the plugin from
 *  its own. Channels are counted from 1.
 */
namespace bambi {

enum class Output { Headphones, Speakers };

/// The bus channels `which` writes, ascending; empty without a layout.
std::vector<int> outputChannels(const DecoderState& d, Output which);

/*  The bus the outputs are placed on: its width, and, under a host that plays the Decoder's outputs on devices of
 *  its own, the channels it keeps for itself -- Live's Main, a copy -- and where one device's channels end. An
 *  output takes no kept channel and plays on one device. A plain plugin host's bus is one device, nothing kept.
 */
struct OutputBus {
    int channels{0};
    std::vector<int> kept;        ///< channels no output may take
    std::vector<int> deviceEnds;  ///< each device's last channel but the last's, ascending; that one ends the bus

    OutputBus(int width = 0) : channels(width) {}  // NOLINT: a plain bus, so a width reads as one
    friend bool operator==(const OutputBus&, const OutputBus&) = default;
};

/// What lighting `which` on `bus` does, against the other output as the state has it.
struct Placement {
    int needs{0};         ///< the highest channel it writes; 0 for none
    bool fits{false};     ///< it has channels, and the bus reaches them
    bool collide{false};  ///< it shares a channel with the other output
    bool otherOn{false};  ///< the other output plays
    bool blocked{false};  ///< where it is, it takes a kept channel or plays on two devices
    bool moves{false};    ///< lit where it is blocked, or on the other's channels, with room elsewhere: to `movedTo`
    bool stops{false};    ///< lit on the other's channels, not blocked, with no room elsewhere: the other stops
    bool noRoom{false};   ///< blocked, with no room elsewhere: it does not light
    /// Its first channel after the move: the nearest start past the other's channels when it lands on them, else
    /// past its own first, else from channel 1. Headphones take an odd pair, as a page picks them.
    int movedTo{0};
    int span{0};  ///< channels from its first to its last, gaps included
};
Placement placeOutput(const DecoderState& d, Output which, const OutputBus& bus);

/// A state after an output is lit or stopped on `bus`, as the page lights it.
struct Lit {
    DecoderState state;
    std::string notice;  ///< why it did not light, or what it moved or stopped; empty when nothing needs saying
};
/// Lighting one that needs more channels than the bus has, or that finds no room clear of the kept channels and
/// on one device, leaves it dark; lighting one where it is blocked, or on the other's channels, moves it where
/// there is room, and stops the other when only the other is in its way.
Lit lightOutput(DecoderState d, Output which, bool on, const OutputBus& bus);

}  // namespace bambi
