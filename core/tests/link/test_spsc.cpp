// SPDX-License-Identifier: GPL-3.0-or-later
#include <atomic>
#include <cstdint>
#include <thread>

#include "bambi/link/spsc.hpp"
#include "doctest.h"

using namespace bambi;

TEST_CASE("the queue hands items over in order, and refuses when full rather than overwriting") {
    SpscQueue<int, 4> q;
    int out = 0;
    CHECK_FALSE(q.pop(out));
    for (int i = 1; i <= 4; ++i) CHECK(q.push(i));
    CHECK(q.full());
    CHECK_FALSE(q.push(5));
    for (int i = 1; i <= 4; ++i) {
        REQUIRE(q.pop(out));
        CHECK(out == i);  // 5 overwrote nothing
    }
    CHECK_FALSE(q.pop(out));
    CHECK_FALSE(q.full());

    for (int round = 0; round < 10; ++round) {  // around the ring several times
        CHECK(q.push(round));
        REQUIRE(q.pop(out));
        CHECK(out == round);
    }
}

TEST_CASE("one producer and one consumer on two threads lose nothing and duplicate nothing") {
    SpscQueue<std::uint32_t, 16> q;  // small, so the producer meets a full queue again and again
    constexpr std::uint32_t kItems = 300'000;
    std::atomic<bool> producerDone{false};
    std::uint64_t refused = 0;

    std::thread producer([&] {
        for (std::uint32_t i = 1; i <= kItems;) {
            if (q.push(i)) {
                ++i;
            } else {
                ++refused;
                std::this_thread::yield();
            }
        }
        producerDone.store(true, std::memory_order_release);
    });

    std::uint32_t expected = 1, out = 0;
    bool inOrder = true;
    while (expected <= kItems) {
        if (q.pop(out)) {
            inOrder = inOrder && out == expected;
            ++expected;
        } else if (producerDone.load(std::memory_order_acquire)) {
            if (!q.pop(out)) break;  // done and empty: anything still missing was lost
            inOrder = inOrder && out == expected;
            ++expected;
        } else {
            std::this_thread::yield();
        }
    }
    producer.join();

    CHECK(inOrder);
    CHECK(expected == kItems + 1);
    CHECK_FALSE(q.pop(out));
    CHECK(refused > 0);  // not vacuous: the full queue was met and refused
}
