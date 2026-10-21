// SPDX-License-Identifier: GPL-3.0-or-later
//
//  State reaching the audio thread without a lock: what is posted is taken whole at a block, and what it replaced
//  is freed on the message thread, never in a block.

#include <atomic>
#include <thread>

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
    const int freedBefore = SnapshotCensus::freedOnAudioThread();
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
    CHECK(SnapshotCensus::freedOnAudioThread() == freedBefore);
}

TEST_CASE("the census counts a state freed inside a block, on that thread only") {
    //  Catches: a census that never counts, so a free on the audio thread goes unseen; one that keeps counting after
    //  the block; and one that blames this thread for another thread's block.
    const int before = SnapshotCensus::freedOnAudioThread();
    {
        auto s = std::make_unique<State>(1);
        const SnapshotCensus::InAudioBlock block;
        s.reset();
    }
    CHECK(SnapshotCensus::freedOnAudioThread() == before + 1);
    std::make_unique<State>(2).reset();  // after the block
    CHECK(SnapshotCensus::freedOnAudioThread() == before + 1);
    std::atomic<bool> inside{false}, done{false};
    std::thread other([&] {
        const SnapshotCensus::InAudioBlock block;
        inside = true;
        while (!done) std::this_thread::yield();
    });
    while (!inside) std::this_thread::yield();
    std::make_unique<State>(3).reset();  // here, while the other thread is in a block
    done = true;
    other.join();
    CHECK(SnapshotCensus::freedOnAudioThread() == before + 1);
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
