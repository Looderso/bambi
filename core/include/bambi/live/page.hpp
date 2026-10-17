// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vector>

#include "bambi/live/protocol.hpp"

/*  Where the routing page puts Live's tracks and groups, in the page's own grid: a column per depth, the deepest on
 *  the left and Main on the right of the top level; a row per track that holds nothing, in Live's order, and each
 *  group on the row midway between its first and its last child. A group that holds nothing takes a row of its
 *  own, as a track does. Returns are not in it: they are laid out apart, as Live shows them.
 *
 *  Rows are counted in rows, a midway one as a half. Allocates; for the message thread.
 */
namespace bambi::live {

struct Place {
    TrackId node{0};
    int column{0};    ///< 0 the leftmost
    double row{0.0};  ///< 0 the topmost
    friend bool operator==(const Place&, const Place&) = default;
};

struct PageLayout {
    std::vector<Place> places;  ///< in Live's order, a group after what it holds
    Place main;                 ///< node 0: Main
    int rows{0};                ///< the rows the tracks take
};

PageLayout layOutPage(const Tree& tree);

}  // namespace bambi::live
