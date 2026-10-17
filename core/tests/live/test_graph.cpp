// SPDX-License-Identifier: GPL-3.0-or-later
//
//  The host's shape, compiled. The steps are played here on arrival times instead of audio: what reaches the
//  output must have taken the same time on every path, and every path must be there once.

#include <algorithm>
#include <random>
#include <string>
#include <vector>

#include "bambi/live/graph.hpp"
#include "doctest.h"

using namespace bambi::live;

namespace {

//  The steps played on when each path's signal gets there, from the loop device: one entry per path.
std::vector<int> arrivals(const Plan& p, const Compiled& c) {
    const auto latency = [&](int slot) { return std::max(0, p.slots[static_cast<std::size_t>(slot)].latency); };
    std::vector<std::vector<int>> at(static_cast<std::size_t>(c.buffers));
    std::vector<int> out;
    for (const Op& op : c.ops) {
        switch (op.step) {
            case Step::Encode: at[static_cast<std::size_t>(op.to)] = {latency(op.slot)}; break;
            case Step::Clear: at[static_cast<std::size_t>(op.to)].clear(); break;
            case Step::Add:
                for (const int t : at[static_cast<std::size_t>(op.from)])
                    at[static_cast<std::size_t>(op.to)].push_back(t + op.delay);
                break;
            case Step::Process:
                for (int& t : at[static_cast<std::size_t>(op.to)]) t += latency(op.slot);
                break;
            case Step::Decode:
                for (const int t : at[static_cast<std::size_t>(op.from)]) out.push_back(t + latency(op.slot));
                break;
        }
    }
    return out;
}

//  How many paths a plan has: every source dry through its buses to Main, and to every return from itself and
//  from each bus above it.
int paths(const Plan& p) {
    int n = 0;
    for (const FieldNode& node : p.nodes) {
        if (node.input < 0) continue;
        int depth = 1;
        for (int at = node.parent; at >= 0; at = p.nodes[static_cast<std::size_t>(at)].parent) ++depth;
        n += 1 + depth * static_cast<int>(p.returns.size());
    }
    return n;
}

//  A plan of random shape and latencies: buses first, each inside an earlier bus or at Main, then sources in any.
Plan randomPlan(std::mt19937& random) {
    std::uniform_int_distribution<int> count(0, 4), length(0, 600), coin(0, 1);
    Plan p;
    const auto slot = [&] {
        p.slots.push_back({length(random)});
        return static_cast<int>(p.slots.size()) - 1;
    };
    const int buses = count(random), sources = count(random) + 1, returns = count(random) % 3;
    for (int b = 0; b < buses; ++b) {
        FieldNode n;
        n.parent = b == 0 || coin(random) ? -1 : std::uniform_int_distribution<int>(0, b - 1)(random);
        for (int i = count(random) % 3; i > 0; --i) n.inserts.push_back(slot());
        p.nodes.push_back(n);
    }
    for (int s = 0; s < sources; ++s) {
        FieldNode n;
        n.track = static_cast<TrackId>(100 + s);
        n.input = 2 * s;
        n.encoder = slot();
        n.parent = buses == 0 || coin(random) ? -1 : std::uniform_int_distribution<int>(0, buses - 1)(random);
        for (int i = count(random) % 3; i > 0; --i) n.inserts.push_back(slot());
        p.nodes.push_back(n);
    }
    for (int r = 0; r < returns; ++r) {
        Return ret;
        for (int i = count(random) % 3; i > 0; --i) ret.chain.push_back(slot());
        p.returns.push_back(ret);
    }
    for (int i = count(random) % 3; i > 0; --i) p.main.push_back(slot());
    p.decoders = {slot()};
    return p;
}

Plan source(int latency) {
    Plan p;
    p.slots = {{latency}, {64}};
    FieldNode n;
    n.track = 1;
    n.input = 0;
    n.encoder = 0;
    p.nodes = {n};
    p.decoders = {1};
    return p;
}

std::vector<Op> without(std::vector<Op> ops, int buffer) {
    for (Op& op : ops) op.delay = 0;
    ops.erase(std::remove_if(ops.begin(), ops.end(), [&](const Op& o) { return o.to == buffer; }), ops.end());
    return ops;
}

}  // namespace

TEST_CASE("every path from the loop device to the output takes the same time, and each is there once") {
    //  Catches: a delay wrong, missing or of the wrong sign anywhere -- at a bus, a return, Main -- a send or a dry
    //  path left out or doubled, or a bus read before what plays into it is done.
    std::mt19937 random(11);
    for (int round = 0; round < 300; ++round) {
        const Plan p = randomPlan(random);
        std::string error;
        const auto c = compile(p, &error);
        REQUIRE_MESSAGE(c.has_value(), error);
        const std::vector<int> got = arrivals(p, *c);
        CHECK(static_cast<int>(got.size()) == paths(p));
        for (const int t : got) CHECK(t == c->latency);
    }
}

TEST_CASE("at every place paths meet, the slowest is not delayed") {
    //  Catches: lining up to anything slower than the slowest path, which plays the whole mix later than it must.
    std::mt19937 random(12);
    for (int round = 0; round < 300; ++round) {
        const Plan p = randomPlan(random);
        const auto c = compile(p);
        REQUIRE(c.has_value());
        std::vector<int> least(static_cast<std::size_t>(c->buffers), -1);
        for (const Op& op : c->ops)
            if (op.step == Step::Add)
                least[static_cast<std::size_t>(op.to)] =
                    least[static_cast<std::size_t>(op.to)] < 0
                        ? op.delay
                        : std::min(least[static_cast<std::size_t>(op.to)], op.delay);
        for (const int l : least) CHECK((l == -1 || l == 0));
    }
}

TEST_CASE("a source that takes no time reaches the output in the decoder's time alone") {
    //  Catches: a meeting place padded by a constant -- the random shapes hide it when every input is slower than the
    //  pad, a source of no latency does not.
    const Plan p = source(0);
    const auto c = compile(p);
    REQUIRE(c.has_value());
    CHECK(c->latency == 64);
    for (const Op& op : c->ops) CHECK(op.delay == 0);
}

TEST_CASE("whatever is read is finished: a send after its node's inserts, a bus after what plays into it") {
    //  Catches: a send taken before the inserts, so the reverb hears the vocal without its echo; or a bus summing a
    //  child before the child's own steps have run. Either way something writes a buffer after it was read.
    std::mt19937 random(13);
    for (int round = 0; round < 300; ++round) {
        const Plan p = randomPlan(random);
        const auto c = compile(p);
        REQUIRE(c.has_value());
        for (std::size_t k = 0; k < c->ops.size(); ++k) {
            const Op& read = c->ops[k];
            if (read.step != Step::Add && read.step != Step::Decode) continue;
            //  a decoder's step writes no buffer: its `to` is which decoder
            for (std::size_t later = k + 1; later < c->ops.size(); ++later)
                if (c->ops[later].step != Step::Decode) CHECK(c->ops[later].to != read.from);
        }
    }
}

TEST_CASE("a send reads its node's level from the table, a node and a return their own where they play on") {
    //  Catches: the table read the wrong way round (return * nodes + node), or a dry path scaled by a send level.
    Plan p = source(0);
    FieldNode second = p.nodes[0];
    second.track = 2;
    second.input = 2;
    p.slots.push_back({0});
    second.encoder = 2;
    p.nodes.push_back(second);
    p.returns = {Return{}, Return{}, Return{}};
    const auto c = compile(p);
    REQUIRE(c.has_value());
    const int nodes = 2, returns = 3;
    for (const Op& op : c->ops) {
        if (op.step != Step::Add) continue;
        if (op.to >= nodes && op.to < nodes + returns)
            CHECK(op.gain == op.from * returns + (op.to - nodes));  // a send
        else
            CHECK(op.gain == nodes * returns + op.from);  // a node or a return, into its parent or Main
    }
    CHECK(c->levels == nodes * returns + nodes + returns);
    //  and in trees with buses inside buses: every way into a bus or Main reads its own
    std::mt19937 random(23);
    for (int round = 0; round < 100; ++round) {
        const Plan q = randomPlan(random);
        const auto d = compile(q);
        REQUIRE(d.has_value());
        const int n = static_cast<int>(q.nodes.size()), r = static_cast<int>(q.returns.size());
        for (const Op& op : d->ops)
            if (op.step == Step::Add && !(op.to >= n && op.to < n + r)) CHECK(op.gain == n * r + op.from);
    }
}

TEST_CASE("a source added at the end leaves every other node's steps as they were") {
    //  Catches: buffers or steps numbered so that a new track moves the others -- every running plugin would be
    //  handed another track's signal.
    std::mt19937 random(14);
    for (int round = 0; round < 100; ++round) {
        Plan p = randomPlan(random);
        p.returns.clear();  // a node's sends are what changes when a node is added; its own steps must not
        const auto before = compile(p);
        FieldNode added;
        added.track = 999;
        added.input = 62;
        p.slots.push_back({7});
        added.encoder = static_cast<int>(p.slots.size()) - 1;
        p.nodes.push_back(added);
        const auto after = compile(p);
        REQUIRE(before.has_value());
        REQUIRE(after.has_value());
        //  everything but Main's sum, and the new node's own steps, is unchanged apart from delays
        std::vector<Op> was = without(before->ops, before->mainBuffer()), is = without(after->ops, after->mainBuffer());
        is.erase(std::remove_if(is.begin(), is.end(),
                                [&](const Op& o) { return o.to == static_cast<int>(p.nodes.size()) - 1; }),
                 is.end());
        for (Op& op : was)
            if (op.step == Step::Decode) op.from = after->mainBuffer();
        CHECK(was == is);
    }
}

TEST_CASE("the shape the first delay compensation was checked on comes out the same") {
    //  Catches: a total that leaves out the main chain or the decoder. Three sources (0, 64, 10), three returns (128,
    //  0, 300), main 64 with no decoder latency: 64 + 300 + 64.
    Plan p;
    for (const int l : {0, 64, 10, 128, 0, 300, 64, 0}) p.slots.push_back({l});
    for (int s = 0; s < 3; ++s) {
        FieldNode n;
        n.track = static_cast<TrackId>(s + 1);
        n.input = 2 * s;
        n.encoder = s;
        p.nodes.push_back(n);
    }
    p.returns = {Return{{3}}, Return{{4}}, Return{{5}}};
    p.main = {6};
    p.decoders = {7};
    const auto c = compile(p);
    REQUIRE(c.has_value());
    CHECK(c->latency == 64 + 300 + 64);
}

TEST_CASE("a plan that does not hold together is refused") {
    //  Catches: a plan played that would read a pair from nowhere, run one plugin in two places, or loop.
    std::string error;
    Plan p = source(0);
    p.nodes[0].input = -1;
    CHECK_FALSE(compile(p, &error).has_value());  // an encoder without a pair

    p = source(0);
    p.nodes[0].encoder = -1;
    CHECK_FALSE(compile(p, &error).has_value());  // a pair without an encoder

    p = source(0);
    p.nodes[0].inserts = {0};
    CHECK_FALSE(compile(p, &error).has_value());  // the encoder's slot again as an insert

    p = source(0);
    p.nodes[0].parent = 5;
    CHECK_FALSE(compile(p, &error).has_value());  // a parent that does not exist

    p = source(0);
    p.nodes.push_back(p.nodes[0]);
    p.slots.push_back({0});
    p.nodes[1].encoder = 2;
    p.nodes[1].parent = 0;
    CHECK_FALSE(compile(p, &error).has_value());  // a parent that is a source

    p = source(0);
    FieldNode a, b;
    a.parent = 2;
    b.parent = 1;
    p.nodes.push_back(a);
    p.nodes.push_back(b);
    CHECK_FALSE(compile(p, &error).has_value());  // two buses inside each other
    CHECK(error.find("cycle") != std::string::npos);

    p = source(0);
    p.decoders = {-1};
    CHECK_FALSE(compile(p, &error).has_value());

    p = source(0);
    p.main = {9};
    CHECK_FALSE(compile(p, &error).has_value());  // a slot out of range

    CHECK(compile(source(0), &error).has_value());
}

TEST_CASE("an Encoder's and an insert's step name the pair their sidechain hears; one without, none") {
    //  Catches: a sidechain pair lost on the way to the step, one given to every slot, or to the wrong one.
    Plan p;
    p.slots = {
        {0, 4}, {0, 5}, {0, -1}, {0, -1}};  // the encoder hearing pair 4, an insert pair 5, an insert, the decoder
    FieldNode n;
    n.track = 1;
    n.input = 0;
    n.encoder = 0;
    n.inserts = {1, 2};
    p.nodes = {n};
    p.decoders = {3};
    const auto c = compile(p);
    REQUIRE(c.has_value());
    int seen = 0;
    for (const Op& op : c->ops)
        if (op.step == Step::Process) {
            CHECK(op.sidechain == (op.slot == 1 ? 5 : -1));
            ++seen;
        } else if (op.step == Step::Encode) {
            CHECK(op.sidechain == 4);
            CHECK(op.from == 0);  // its own pair, apart from its sidechain's
            ++seen;
        }
    CHECK(seen == 3);
}

TEST_CASE("a source's width reaches its step, and a source neither one nor two channels wide is refused") {
    //  Catches: a mono input encoded as a pair -- the channel beside it would be heard -- or a width of 3 taken.
    Plan p;
    p.slots = {{0, 3, 1}, {0, -1}};
    FieldNode n;
    n.track = 1;
    n.input = 20;
    n.width = 1;
    n.encoder = 0;
    p.nodes = {n};
    p.decoders = {1};
    const auto c = compile(p);
    REQUIRE(c.has_value());
    const auto encode =
        std::find_if(c->ops.begin(), c->ops.end(), [](const Op& op) { return op.step == Step::Encode; });
    REQUIRE(encode != c->ops.end());
    CHECK((encode->from == 20 && encode->width == 1 && encode->sidechain == 3 && encode->sidechainWidth == 1));
    p.nodes[0].width = 3;
    CHECK_FALSE(compile(p).has_value());
}

TEST_CASE("every decoder hears Main after its chain, each by its place, the plan as late as the slowest") {
    //  Catches: a second decoder left without a step, two writing the same output, one before Main's chain,
    //  the latency of the first decoder taken for all.
    Plan p;
    p.slots = {{0, -1}, {10, -1}, {64, -1}, {200, -1}};
    FieldNode n;
    n.track = 1;
    n.input = 0;
    n.encoder = 0;
    p.nodes = {n};
    p.main = {1};
    p.decoders = {2, 3};
    const auto c = compile(p);
    REQUIRE(c.has_value());
    std::vector<int> decoded;
    for (const Op& op : c->ops)
        if (op.step == Step::Decode) {
            CHECK(op.from == c->mainBuffer());
            decoded.push_back(op.to);
        }
    CHECK(decoded == std::vector<int>{0, 1});
    CHECK(c->ops.back().step == Step::Decode);
    CHECK(c->latency == 10 + 200);
}
