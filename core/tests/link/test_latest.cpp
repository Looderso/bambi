// SPDX-License-Identifier: GPL-3.0-or-later
//
//  LatestValue is the only thing the audio thread touches on its way to the link bus, so its
//  two promises are tested directly: never a torn value, and never an older value after a
//  newer one.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include "bambi/link/latest.hpp"
#include "doctest.h"

using namespace bambi;

namespace {

//  b and c are exact multiples of a, so "one write or halves of two" is an integer question.
struct Sample {
    std::int64_t a{0}, b{0}, c{0};
};

Sample sampleAt(std::int64_t n) { return {n, 2 * n, 3 * n}; }

}  // namespace

TEST_CASE("nothing to read until something is written") {
    //  Catches: latest() handing out a slot before anything was published, which reads as a write of zeros.
    LatestValue<Sample> v;
    Sample out = sampleAt(-1);
    CHECK_FALSE(v.read(out));
    CHECK(out.a == -1);  // and a failed read leaves the caller's copy alone
}

TEST_CASE("a read takes the newest write and skips the ones in between") {
    //  Catches: a read that leaves the fresh flag set, so the same value is handed over again as new.
    LatestValue<Sample> v;
    v.write(sampleAt(1));
    v.write(sampleAt(2));
    v.write(sampleAt(3));

    Sample out;
    REQUIRE(v.read(out));
    CHECK(out.a == 3);

    //  Nothing newer: the reader is told so, rather than handed 3 a second time, and keeps
    //  the value it has.
    CHECK_FALSE(v.read(out));
    CHECK(out.a == 3);

    v.write(sampleAt(4));
    REQUIRE(v.read(out));
    CHECK(out.a == 4);
}

TEST_CASE("interleaved writes and reads always deliver the latest value") {
    //  Catches: a publish that does not mark the value fresh, so the reader never sees it.
    LatestValue<Sample> v;
    Sample out;
    for (int n = 1; n < 1000; ++n) {
        v.write(sampleAt(n));
        if (n % 3 == 0) {
            REQUIRE(v.read(out));
            CHECK(out.a == n);
        }
    }
}

TEST_CASE("under contention a reader never sees a torn value, and never goes backwards") {
    //  Catches: a publish that hands its slot over without taking another, so the writer goes on writing the slot the
    //  reader copies.
    LatestValue<Sample> v;
    std::atomic<bool> stop{false};
    std::atomic<std::int64_t> lastWritten{0};

    std::thread writer([&] {
        std::int64_t n = 1;
        while (!stop.load(std::memory_order_relaxed)) {
            v.write(sampleAt(n));
            lastWritten.store(n, std::memory_order_relaxed);
            ++n;
        }
    });

    std::int64_t reads = 0, torn = 0, backwards = 0, previous = 0;
    Sample out;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < until) {
        if (!v.read(out)) continue;
        ++reads;
        if (out.b != 2 * out.a || out.c != 3 * out.a) ++torn;
        if (out.a < previous) ++backwards;
        previous = out.a;
    }
    stop.store(true);
    writer.join();

    //  And nothing is lost at the end: the final write is either what was read last, or is
    //  waiting to be read now.
    v.read(out);
    CHECK(out.a == lastWritten.load());

    INFO("reads ", reads);
    CHECK(reads > 1000);  // the test actually exercised contention
    CHECK(torn == 0);
    CHECK(backwards == 0);
}

TEST_CASE("a value published in place is read in place, and the reader keeps its slot until it asks again") {
    //  Catches: latest() handing back a slot the writer may be filling, or hasFresh() cleared by anyone but the reader.
    LatestValue<std::array<int, 4>> v;
    CHECK(v.latest() == nullptr);
    CHECK_FALSE(v.hasFresh());
    v.staging() = {1, 2, 3, 4};
    v.publish();
    CHECK(v.hasFresh());
    const std::array<int, 4>* first = v.latest();
    REQUIRE(first != nullptr);
    CHECK((*first)[2] == 3);
    CHECK_FALSE(v.hasFresh());
    CHECK(v.latest() == nullptr);
    v.staging() = {5, 6, 7, 8};
    v.publish();
    v.staging() = {9, 10, 11, 12};  // the writer's new slot is never the reader's
    CHECK(&v.staging() != first);
    CHECK((*first)[0] == 1);
    const std::array<int, 4>* second = v.latest();
    REQUIRE(second != nullptr);
    CHECK((*second)[0] == 5);
    CHECK(second != first);
}
