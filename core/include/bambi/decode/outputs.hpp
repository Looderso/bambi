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

/// What lighting `which` on a bus of `bus` channels does, against the other output as the state has it.
struct Placement {
    int needs{0};         ///< the highest channel it writes; 0 for none
    bool fits{false};     ///< it has channels, and the bus reaches them
    bool collide{false};  ///< it shares a channel with the other output
    bool otherOn{false};  ///< the other output plays
    bool moves{false};    ///< lit on the other's channels with room past them: it moves to `movedTo`
    bool stops{false};    ///< lit on the other's channels with no room: the other stops
    int movedTo{0};       ///< its first channel after the move; headphones take an odd pair, as a page picks them
    int span{0};          ///< channels from its first to its last, gaps included
};
Placement placeOutput(const DecoderState& d, Output which, int bus);

/// A state after an output is lit or stopped on a bus of `bus` channels, as the page lights it.
struct Lit {
    DecoderState state;
    std::string notice;  ///< why it did not light, or what it moved or stopped; empty when nothing needs saying
};
/// Lighting one that needs more channels than the bus has leaves it dark; lighting one on the other's channels
/// moves it past them while the bus has room, and stops the other when it has not.
Lit lightOutput(DecoderState d, Output which, bool on, int bus);

}  // namespace bambi
