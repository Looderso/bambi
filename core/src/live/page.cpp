// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/live/page.hpp"

#include <algorithm>

namespace bambi::live {

namespace {

bool shown(const Node& n) { return n.kind != Node::Kind::Return; }

//  How deep `n` sits: 0 at the top. A parent the tree does not have counts as the top; the depth guards a cycle a
//  device could send.
int depthOf(const Tree& tree, const Node& n) {
    int depth = 0;
    std::optional<TrackId> up = n.parent;
    while (up && depth < 64) {
        const auto parent =
            std::find_if(tree.nodes.begin(), tree.nodes.end(), [&](const Node& p) { return p.id == *up; });
        if (parent == tree.nodes.end()) break;
        ++depth;
        up = parent->parent;
    }
    return depth;
}

}  // namespace

PageLayout layOutPage(const Tree& tree) {
    PageLayout out;
    int deepest = 0;
    for (const Node& n : tree.nodes)
        if (shown(n)) deepest = std::max(deepest, depthOf(tree, n));
    const auto columnOf = [&](int depth) { return deepest - depth; };

    //  `n` and what it holds: returns the rows it spans, its own the middle of them
    const auto place = [&](const auto& self, const Node& n, int depth) -> std::pair<double, double> {
        std::pair<double, double> span{0.0, -1.0};
        if (depth < 64)
            for (const Node& child : tree.nodes)
                if (shown(child) && child.parent == n.id && child.id != n.id) {
                    const auto [first, last] = self(self, child, depth + 1);
                    span = span.second < span.first ? std::pair{first, last} : std::pair{span.first, last};
                }
        const bool holds = span.second >= span.first;
        const double row = holds ? (span.first + span.second) / 2.0 : static_cast<double>(out.rows++);
        out.places.push_back({n.id, columnOf(depth), row});
        return holds ? span : std::pair{row, row};
    };

    double first = 0.0, last = -1.0;
    for (const Node& n : tree.nodes) {
        if (!shown(n)) continue;
        const bool top = !n.parent || std::none_of(tree.nodes.begin(), tree.nodes.end(),
                                                   [&](const Node& p) { return p.id == *n.parent; });
        if (!top) continue;
        const auto [a, b] = place(place, n, 0);
        const double row = (a + b) / 2.0;
        if (last < first) first = row;
        last = row;
    }
    out.main = {0, columnOf(-1), last < first ? 0.0 : (first + last) / 2.0};
    return out;
}

}  // namespace bambi::live
