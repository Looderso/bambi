// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vector>

#include "bambi/live/field.hpp"
#include "bambi/live/protocol.hpp"

/*  What the routing page shows of Live's tracks and groups, and what its two acts on them do to the shape.
 *
 *  The page lists them as Live does: in Live's order, a group before what it holds, each line one deeper than its
 *  group -- the group Live files it under, whatever its routing. What it is, and what an Encoder on it does, follow where
 *  its audio goes: a member routed past its group by hand is its own path, encoded on its own and never with the group. Returns are not among them: they are listed apart, beside Main. A node whose group the tree does not have
 *  counts as at the top.
 *
 *  Each is, as the field has it now: in Live; a source of its own, encoded; a bus, a group holding something taken;
 *  or inside a group the field encodes whole. An Encoder added to a node takes it alone, at any depth; its siblings
 *  stay where they are. Inside a group encoded whole, that group opens on the way down: what else it held is taken
 *  in its place, so nothing that was heard in the field goes back to Live. Taking a node's Encoder away gives it
 *  back to Live, alone; "encode whole" on a bus takes the group whole, and what it held with it.
 *
 *  Allocates; for the message thread.
 */
namespace bambi::live {

struct Line {
    TrackId node{0};
    int depth{0};  ///< 0 at the top
    friend bool operator==(const Line&, const Line&) = default;
};

std::vector<Line> linesOf(const Tree& tree);

enum class Role { Live, Source, Bus, InGroup };
Role roleOf(const Tree& tree, const Field& field, TrackId node);

void addEncoder(Shape& shape, const Tree& tree, TrackId node);
void removeEncoder(Shape& shape, const Tree& tree, const Field& field, TrackId node);

}  // namespace bambi::live
