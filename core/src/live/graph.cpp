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

//  A node's wires, its default where the plan left them unset.
std::vector<PlanWire> wiresOfNode(const FieldNode& n) {
    if (n.wires) return *n.wires;
    return {n.parent >= 0 ? PlanWire{PlanWire::To::Node, n.parent} : PlanWire{PlanWire::To::Main, 0}};
}
std::vector<PlanWire> wiresOfReturn(const Return& r) {
    return r.wires ? *r.wires : std::vector<PlanWire>{{PlanWire::To::Main, 0}};
}
std::vector<PlanWire> wiresOfMain(const Plan& p) {
    if (p.mainWires) return *p.mainWires;
    std::vector<PlanWire> all;
    all.reserve(p.decoders.size());
    for (std::size_t k = 0; k < p.decoders.size(); ++k) all.push_back({PlanWire::To::Decoder, static_cast<int>(k)});
    return all;
}

bool valid(const Plan& p, std::string* error) {
    const int slots = static_cast<int>(p.slots.size()), nodes = static_cast<int>(p.nodes.size());
    const int returns = static_cast<int>(p.returns.size()), decoders = static_cast<int>(p.decoders.size());
    std::vector<int> uses(static_cast<std::size_t>(slots), 0);
    const auto use = [&](int slot) {
        if (slot < 0 || slot >= slots) return false;
        return ++uses[static_cast<std::size_t>(slot)] == 1;
    };
    const auto isBus = [&](int i) { return i >= 0 && i < nodes && p.nodes[static_cast<std::size_t>(i)].input < 0; };
    for (int i = 0; i < nodes; ++i) {
        const FieldNode& n = p.nodes[static_cast<std::size_t>(i)];
        const bool source = n.input >= 0 || n.encoder >= 0;
        if (source && (n.input < 0 || n.encoder < 0)) return fail(error, "a source needs an input and an encoder");
        if (source && n.width != 1 && n.width != 2) return fail(error, "a source is one channel wide or two");
        if (source && !use(n.encoder)) return fail(error, "an encoder slot is out of range or used twice");
        for (const int s : n.inserts)
            if (!use(s)) return fail(error, "an insert slot is out of range or used twice");
        if (n.parent >= nodes || n.parent < -1) return fail(error, "a parent that does not exist");
        if (n.parent >= 0 && !isBus(n.parent)) return fail(error, "a parent that is a source, not a bus");
        for (const PlanWire& w : wiresOfNode(n)) {
            const bool there = w.to == PlanWire::To::Main ||
                               (w.to == PlanWire::To::Node && isBus(w.index) && w.index != i) ||
                               (w.to == PlanWire::To::Return && w.index >= 0 && w.index < returns) ||
                               (w.to == PlanWire::To::Decoder && w.index >= 0 && w.index < decoders);
            if (!there) return fail(error, "a node wired to a bus, a return or a decoder that is not there");
        }
    }
    //  every node reaches the end of its wires into buses within as many steps as there are nodes
    std::vector<int> depth(static_cast<std::size_t>(nodes), -1);  // -1 unknown, -2 on the way
    const std::function<bool(int)> settles = [&](int i) {
        auto& d = depth[static_cast<std::size_t>(i)];
        if (d == -2) return false;
        if (d >= 0) return true;
        d = -2;
        for (const PlanWire& w : wiresOfNode(p.nodes[static_cast<std::size_t>(i)]))
            if (w.to == PlanWire::To::Node && !settles(w.index)) return false;
        d = 0;
        return true;
    };
    for (int i = 0; i < nodes; ++i)
        if (!settles(i)) return fail(error, "a cycle of buses");
    for (const Return& r : p.returns) {
        for (const int s : r.chain)
            if (!use(s)) return fail(error, "a return's slot is out of range or used twice");
        for (const PlanWire& w : wiresOfReturn(r))
            if (w.to != PlanWire::To::Main && !(w.to == PlanWire::To::Decoder && w.index >= 0 && w.index < decoders))
                return fail(error, "a return wired elsewhere than Main or a decoder");
    }
    for (const int s : p.main)
        if (!use(s)) return fail(error, "a main chain slot is out of range or used twice");
    if (p.decoders.empty()) return fail(error, "no decoder");
    for (const int d : p.decoders)
        if (!use(d)) return fail(error, "a decoder's slot is out of range or used twice");
    for (const PlanWire& w : wiresOfMain(p))
        if (w.to != PlanWire::To::Decoder || w.index < 0 || w.index >= decoders)
            return fail(error, "Main wired elsewhere than a decoder");
    for (const Slot& s : p.slots)
        if (s.sidechain >= 0 && s.sidechainWidth != 1 && s.sidechainWidth != 2)
            return fail(error, "a sidechain is one channel wide or two");
    return true;
}

}  // namespace

std::vector<WireOf> wiresOf(const Plan& p) {
    std::vector<WireOf> out;
    for (std::size_t i = 0; i < p.nodes.size(); ++i)
        for (const PlanWire& w : wiresOfNode(p.nodes[i])) out.push_back({WireOf::From::Node, static_cast<int>(i), w});
    for (std::size_t r = 0; r < p.returns.size(); ++r)
        for (const PlanWire& w : wiresOfReturn(p.returns[r]))
            out.push_back({WireOf::From::Return, static_cast<int>(r), w});
    for (const PlanWire& w : wiresOfMain(p)) out.push_back({WireOf::From::Main, 0, w});
    return out;
}

std::optional<Compiled> compile(const Plan& p, std::string* error) {
    const auto sidechainOf = [&](int slot) { return p.slots[static_cast<std::size_t>(slot)].sidechain; };
    const auto sidechainWidthOf = [&](int slot) { return p.slots[static_cast<std::size_t>(slot)].sidechainWidth; };
    if (!valid(p, error)) return std::nullopt;
    const int nodes = static_cast<int>(p.nodes.size()), returns = static_cast<int>(p.returns.size());
    const int decoders = static_cast<int>(p.decoders.size());
    const auto latency = [&](int slot) { return std::max(0, p.slots[static_cast<std::size_t>(slot)].latency); };
    const auto chained = [&](const std::vector<int>& chain) {
        int sum = 0;
        for (const int s : chain) sum += latency(s);
        return sum;
    };

    Compiled c;
    c.decoders = decoders;
    c.buffers = nodes + returns + 1 + decoders;
    const int mainBuffer = c.mainBuffer();
    const auto bufferOf = [&](WireOf::From from, int index) {
        return from == WireOf::From::Node ? index : from == WireOf::From::Return ? nodes + index : mainBuffer;
    };
    //  what is wired into each place that sums: its buffer, and the entry of each wire, in the table's order
    struct In {
        int buffer{0};
        int entry{0};
    };
    std::vector<std::vector<In>> intoNode(static_cast<std::size_t>(nodes)),
        intoReturn(static_cast<std::size_t>(returns)), intoMix(static_cast<std::size_t>(decoders));
    std::vector<In> intoMain;
    const std::vector<WireOf> wires = wiresOf(p);
    for (std::size_t e = 0; e < wires.size(); ++e) {
        const WireOf& w = wires[e];
        const In in{bufferOf(w.from, w.index), static_cast<int>(e)};
        switch (w.wire.to) {
            case PlanWire::To::Node: intoNode[static_cast<std::size_t>(w.wire.index)].push_back(in); break;
            case PlanWire::To::Return: intoReturn[static_cast<std::size_t>(w.wire.index)].push_back(in); break;
            case PlanWire::To::Main: intoMain.push_back(in); break;
            case PlanWire::To::Decoder: intoMix[static_cast<std::size_t>(w.wire.index)].push_back(in); break;
        }
    }
    c.levels = static_cast<int>(wires.size());

    //  when each buffer's output is ready, from the loop device; -1 not yet played
    std::vector<int> out(static_cast<std::size_t>(c.buffers), -1);
    const auto sum = [&](int to, const std::vector<In>& ins) {
        int ready = 0;
        for (const In& in : ins) ready = std::max(ready, out[static_cast<std::size_t>(in.buffer)]);
        c.ops.push_back({Step::Clear, -1, -1, to, 0, -1});
        for (const In& in : ins)
            c.ops.push_back({Step::Add, -1, in.buffer, to, ready - out[static_cast<std::size_t>(in.buffer)], in.entry});
        return ready;
    };
    const auto process = [&](int buffer, const std::vector<int>& chain) {
        for (const int s : chain)
            c.ops.push_back({Step::Process, s, -1, buffer, 0, -1, sidechainOf(s), 2, sidechainWidthOf(s)});
    };

    //  Each node after everything wired into it: a source encodes where it arrives, a bus sums; then its inserts.
    const std::function<void(int)> play = [&](int i) {
        if (out[static_cast<std::size_t>(i)] >= 0) return;
        const FieldNode& n = p.nodes[static_cast<std::size_t>(i)];
        for (const In& in : intoNode[static_cast<std::size_t>(i)]) play(in.buffer);
        int ready = 0;
        if (n.input >= 0) {
            c.ops.push_back({Step::Encode, n.encoder, n.input, i, 0, -1, sidechainOf(n.encoder), n.width,
                             sidechainWidthOf(n.encoder)});
            ready = latency(n.encoder);
        } else {
            ready = sum(i, intoNode[static_cast<std::size_t>(i)]);
        }
        process(i, n.inserts);
        out[static_cast<std::size_t>(i)] = ready + chained(n.inserts);
    };
    for (int i = 0; i < nodes; ++i) play(i);

    for (int r = 0; r < returns; ++r) {
        const auto& chain = p.returns[static_cast<std::size_t>(r)].chain;
        const int ready = sum(nodes + r, intoReturn[static_cast<std::size_t>(r)]);
        process(nodes + r, chain);
        out[static_cast<std::size_t>(nodes + r)] = ready + chained(chain);
    }
    const int ready = sum(mainBuffer, intoMain);
    process(mainBuffer, p.main);
    out[static_cast<std::size_t>(mainBuffer)] = ready + chained(p.main);

    //  every mix held to one time: the slowest input of any
    int mixed = 0;
    for (const auto& ins : intoMix)
        for (const In& in : ins) mixed = std::max(mixed, out[static_cast<std::size_t>(in.buffer)]);
    int slowest = 0;
    for (int k = 0; k < decoders; ++k) {
        const int mix = mainBuffer + 1 + k;
        c.ops.push_back({Step::Clear, -1, -1, mix, 0, -1});
        for (const In& in : intoMix[static_cast<std::size_t>(k)])
            c.ops.push_back(
                {Step::Add, -1, in.buffer, mix, mixed - out[static_cast<std::size_t>(in.buffer)], in.entry});
        c.ops.push_back({Step::Decode, p.decoders[static_cast<std::size_t>(k)], mix, k, 0, -1});
        slowest = std::max(slowest, latency(p.decoders[static_cast<std::size_t>(k)]));
    }
    c.latency = mixed + slowest;
    return c;
}

}  // namespace bambi::live
