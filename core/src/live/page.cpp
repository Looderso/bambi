// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/live/page.hpp"

#include <algorithm>

namespace bambi::live {

namespace {

bool listed(const Node& n) { return n.kind != Node::Kind::Return; }

//  It plays towards Main with audio: what the field can take, as layOut has it.
bool reaches(const Node& n) { return n.path == Node::Path::Main && n.kind != Node::Kind::Return && n.audio; }

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

bool has(const std::vector<TrackId>& ids, TrackId id) { return std::find(ids.begin(), ids.end(), id) != ids.end(); }

//  Every mark below `group` taken away: a group taken whole holds nothing taken of its own.
void clearBelow(Shape& shape, const Tree& tree, TrackId group) {
    std::erase_if(shape.taken, [&](TrackId id) {
        const Node* n = find(tree, id);
        if (n == nullptr) return false;
        const auto up = groupsOf(tree, *n);
        return std::find(up.begin(), up.end(), group) != up.end();
    });
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
    //  inside a group encoded whole: every group from it down to `node` opens, what else each held taken in its place
    const std::vector<TrackId> groups = groupsOf(tree, *n);  // the nearest first
    int outer = -1;
    for (std::size_t i = 0; i < groups.size(); ++i)
        if (has(shape.taken, groups[i])) outer = static_cast<int>(i);
    for (int i = outer; i >= 0; --i) {
        const TrackId g = groups[static_cast<std::size_t>(i)];
        const TrackId toward = i > 0 ? groups[static_cast<std::size_t>(i - 1)] : node;
        std::erase(shape.taken, g);
        for (const Node& child : tree.nodes)
            if (child.parent == g && child.id != g && child.id != toward && reaches(child)) put(shape.taken, child.id);
    }
    put(shape.taken, node);
    clearBelow(shape, tree, node);
}

void removeEncoder(Shape& shape, const Tree& tree, const Field& field, TrackId node) {
    if (roleOf(tree, field, node) == Role::Bus) {
        put(shape.taken, node);
        clearBelow(shape, tree, node);
        return;
    }
    clearBelow(shape, tree, node);  // a group given back takes nothing hidden inside it along
    std::erase(shape.taken, node);
}

}  // namespace bambi::live
