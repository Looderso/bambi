// SPDX-License-Identifier: GPL-3.0-or-later
//
//  State reaching the audio thread without a lock: what is posted is taken whole at a block, and what it replaced
//  is freed on the message thread, never in a block.

#include "bambi/link/handoff.hpp"
#include "doctest.h"

using bambi::Handoff;
using bambi::SnapshotCensus;

namespace {

struct State : SnapshotCensus {
    explicit State(int v) : value(v) {}
    int value;
};

}  // namespace

TEST_CASE("a posted state is taken at the next block, and the one it replaced is freed outside it") {
    //  Catches: an adopt that frees the state it replaces, or one that never retires it -- it would leak, or be
    //  freed where the audio thread runs.
    const int before = SnapshotCensus::live();
    {
        Handoff<State> h;
        h.begin(std::make_unique<State>(1));
        CHECK(h.adopt() == nullptr);  // nothing posted
        (void)h.post(std::make_unique<State>(2));
        CHECK(h.current()->value == 1);  // not before the block takes it
        {
            const SnapshotCensus::InAudioBlock block;
            REQUIRE(h.adopt() != nullptr);
        }
        CHECK(h.current()->value == 2);
        CHECK(SnapshotCensus::live() == before + 2);  // the first, retired, still alive
        h.freeRetired();
        CHECK(SnapshotCensus::live() == before + 1);
    }
    CHECK(SnapshotCensus::live() == before);
    CHECK(SnapshotCensus::freedOnAudioThread() == 0);
}

TEST_CASE("a state posted over one never taken hands that one back, and only the newest is taken") {
    //  Catches: a post that drops what was pending without handing it back, or a block that takes an older one.
    Handoff<State> h;
    h.begin(std::make_unique<State>(1));
    (void)h.post(std::make_unique<State>(2));
    const auto back = h.post(std::make_unique<State>(3));
    REQUIRE(back != nullptr);
    CHECK(back->value == 2);
    CHECK(h.pending());
    REQUIRE(h.adopt() != nullptr);
    CHECK(h.current()->value == 3);
    CHECK_FALSE(h.pending());
}
