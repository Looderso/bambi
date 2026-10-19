// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/live/page.hpp"

#include <algorithm>

namespace bambi::live {

namespace {

bool listed(const Node& n) { return n.kind != Node::Kind::Return; }

const Node* find(const Tree& tree, TrackId id) {
    const auto n = std::find_if(tree.nodes.begin(), tree.nodes.end(), [&](const Node& x) { return x.id == id; });
    return n == tree.nodes.end() ? nullptr : &*n;
}

//  The groups `n`'s audio goes through, the nearest first, as far as the tree has them -- by where it plays, not where Live
//  files it, since what a group encodes is what plays into it. The count guards a cycle a device could send.
std::vector<TrackId> groupsOf(const Tree& tree, const Node& n) {
    std::vector<TrackId> out;
    for (std::optional<TrackId> up = n.parent; up && out.size() < 64;) {
        const Node* p = find(tree, *up);
        if (p == nullptr) break;
        out.push_back(p->id);
        up = p->parent;
    }
    return out;
}

void put(std::vector<TrackId>& ids, TrackId id) {
    if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
}

}  // namespace

std::vector<Line> linesOf(const Tree& tree) {
    std::vector<Line> out;
    const auto add = [&](const auto& self, const Node& n, int depth) -> void {
        out.push_back({n.id, depth});
        if (depth < 64)
            for (const Node& child : tree.nodes)
                if (listed(child) && filedUnder(child) == n.id && child.id != n.id) self(self, child, depth + 1);
    };
    //  listed where Live files it, not where its audio goes: a member routed by hand past its group is still its member here
    for (const Node& n : tree.nodes)
        if (listed(n) && (!filedUnder(n) || find(tree, *filedUnder(n)) == nullptr)) add(add, n, 0);
    return out;
}

Role roleOf(const Tree& tree, const Field& field, TrackId node) {
    for (const Part& p : field.parts)
        if (p.node == node) return p.bus ? Role::Bus : Role::Source;
    if (const Node* n = find(tree, node))
        for (const TrackId g : groupsOf(tree, *n))
            for (const Part& p : field.parts)
                if (p.node == g && !p.bus) return Role::InGroup;
    return Role::Live;
}

void addEncoder(Shape& shape, const Tree& tree, TrackId node) {
    const Node* n = find(tree, node);
    if (n == nullptr) return;
    const std::vector<TrackId> groups = groupsOf(tree, *n);
    for (const TrackId g : groups) put(shape.split, g);
    put(shape.taken, groups.empty() ? node : groups.back());
}

void removeEncoder(Shape& shape, const Tree& tree, const Field& field, TrackId node) {
    if (roleOf(tree, field, node) == Role::Bus) {
        std::erase(shape.split, node);
        return;
    }
    std::erase(shape.taken, node);
}

}  // namespace bambi::live
