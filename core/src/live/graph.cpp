// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/live/graph.hpp"

#include <algorithm>
#include <functional>

namespace bambi::live {

namespace {

bool fail(std::string* error, const std::string& why) {
    if (error != nullptr) *error = why;
    return false;
}

bool valid(const Plan& p, std::string* error) {
    const int slots = static_cast<int>(p.slots.size()), nodes = static_cast<int>(p.nodes.size());
    std::vector<int> uses(static_cast<std::size_t>(slots), 0);
    const auto use = [&](int slot) {
        if (slot < 0 || slot >= slots) return false;
        return ++uses[static_cast<std::size_t>(slot)] == 1;
    };
    for (int i = 0; i < nodes; ++i) {
        const FieldNode& n = p.nodes[static_cast<std::size_t>(i)];
        const bool source = n.pair >= 0 || n.encoder >= 0;
        if (source && (n.pair < 0 || n.encoder < 0)) return fail(error, "a source needs a pair and an encoder");
        if (source && !use(n.encoder)) return fail(error, "an encoder slot is out of range or used twice");
        for (const int s : n.inserts)
            if (!use(s)) return fail(error, "an insert slot is out of range or used twice");
        if (n.parent >= nodes || n.parent < -1) return fail(error, "a parent that does not exist");
        if (n.parent >= 0 && p.nodes[static_cast<std::size_t>(n.parent)].pair >= 0)
            return fail(error, "a parent that is a source, not a bus");
        //  every chain of parents reaches Main within as many steps as there are nodes
        int at = i;
        for (int steps = 0; at >= 0; ++steps) {
            if (steps > nodes) return fail(error, "a cycle of buses");
            at = p.nodes[static_cast<std::size_t>(at)].parent;
        }
    }
    for (const Return& r : p.returns)
        for (const int s : r.chain)
            if (!use(s)) return fail(error, "a return's slot is out of range or used twice");
    for (const int s : p.main)
        if (!use(s)) return fail(error, "a main chain slot is out of range or used twice");
    if (p.decoder < 0) return fail(error, "no decoder");
    if (!use(p.decoder)) return fail(error, "the decoder's slot is out of range or used twice");
    return true;
}

}  // namespace

std::optional<Compiled> compile(const Plan& p, std::string* error) {
    if (!valid(p, error)) return std::nullopt;
    const int nodes = static_cast<int>(p.nodes.size()), returns = static_cast<int>(p.returns.size());
    const auto latency = [&](int slot) { return std::max(0, p.slots[static_cast<std::size_t>(slot)].latency); };
    const auto chained = [&](const std::vector<int>& chain) {
        int sum = 0;
        for (const int s : chain) sum += latency(s);
        return sum;
    };

    std::vector<std::vector<int>> children(static_cast<std::size_t>(nodes));
    std::vector<int> top;
    for (int i = 0; i < nodes; ++i) {
        const int parent = p.nodes[static_cast<std::size_t>(i)].parent;
        (parent < 0 ? top : children[static_cast<std::size_t>(parent)]).push_back(i);
    }

    Compiled c;
    c.buffers = nodes + returns + 1;
    std::vector<int> out(static_cast<std::size_t>(nodes), 0);  // when each node's output is ready, from the loop device

    //  Each node after its children: a source encodes where it arrives, a bus sums what plays into it, each slowest
    //  input by nothing; then its inserts.
    const std::function<void(int)> play = [&](int i) {
        const FieldNode& n = p.nodes[static_cast<std::size_t>(i)];
        const auto& kids = children[static_cast<std::size_t>(i)];
        for (const int k : kids) play(k);
        int ready = 0;
        if (n.pair >= 0) {
            c.ops.push_back({Step::Encode, n.encoder, n.pair, i, 0, -1});
            ready = latency(n.encoder);
        } else {
            for (const int k : kids) ready = std::max(ready, out[static_cast<std::size_t>(k)]);
            c.ops.push_back({Step::Clear, -1, -1, i, 0, -1});
            for (const int k : kids)
                c.ops.push_back({Step::Add, -1, k, i, ready - out[static_cast<std::size_t>(k)], -1});
        }
        for (const int s : n.inserts) c.ops.push_back({Step::Process, s, -1, i, 0, -1});
        out[static_cast<std::size_t>(i)] = ready + chained(n.inserts);
    };
    for (const int t : top) play(t);

    //  every node in the field sends to every return, after its inserts, at the level its table entry holds
    int slowestNode = 0;
    for (int i = 0; i < nodes; ++i) slowestNode = std::max(slowestNode, out[static_cast<std::size_t>(i)]);
    std::vector<int> returned(static_cast<std::size_t>(returns), 0);
    for (int r = 0; r < returns; ++r) {
        const int buffer = nodes + r;
        c.ops.push_back({Step::Clear, -1, -1, buffer, 0, -1});
        for (int i = 0; i < nodes; ++i)
            c.ops.push_back(
                {Step::Add, -1, i, buffer, slowestNode - out[static_cast<std::size_t>(i)], i * returns + r});
        for (const int s : p.returns[static_cast<std::size_t>(r)].chain)
            c.ops.push_back({Step::Process, s, -1, buffer, 0, -1});
        returned[static_cast<std::size_t>(r)] =
            (nodes > 0 ? slowestNode : 0) + chained(p.returns[static_cast<std::size_t>(r)].chain);
    }

    //  Main: what plays to it and every return, each slowest by nothing; the main chain; the decoder
    int ready = 0;
    for (const int t : top) ready = std::max(ready, out[static_cast<std::size_t>(t)]);
    for (int r = 0; r < returns; ++r) ready = std::max(ready, returned[static_cast<std::size_t>(r)]);
    const int main = c.mainBuffer();
    c.ops.push_back({Step::Clear, -1, -1, main, 0, -1});
    for (const int t : top) c.ops.push_back({Step::Add, -1, t, main, ready - out[static_cast<std::size_t>(t)], -1});
    for (int r = 0; r < returns; ++r)
        c.ops.push_back({Step::Add, -1, nodes + r, main, ready - returned[static_cast<std::size_t>(r)], -1});
    for (const int s : p.main) c.ops.push_back({Step::Process, s, -1, main, 0, -1});
    c.ops.push_back({Step::Decode, p.decoder, main, -1, 0, -1});
    c.latency = ready + chained(p.main) + latency(p.decoder);
    return c;
}

}  // namespace bambi::live
