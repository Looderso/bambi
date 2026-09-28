// SPDX-License-Identifier: GPL-3.0-or-later
//
//  The session directory: how a freshly inserted instance finds its project.
//  Time is passed in explicitly, so lapsing is tested exactly and without sleeping.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "bambi/link/directory.hpp"
#include "bambi/link/link.hpp"
#include "doctest.h"

using namespace bambi;

namespace {

/*  A directory nobody else is using. Tests must never touch the machine's real one: a plugin
 *  running on this machine may be using it right now. */
struct TestDirectory {
    std::string name{"/bambi.dt." + Uuid::generate().toString().substr(0, 8)};
    ~TestDirectory() { LinkDirectory::unlink(name); }
};

constexpr std::uint64_t kT0 = 1'000'000'000ull;  // an arbitrary "now", in microseconds

bool lists(const std::vector<LinkSessionInfo>& live, const Uuid& s) {
    return std::any_of(live.begin(), live.end(), [&](const LinkSessionInfo& i) { return i.session == s; });
}

}  // namespace

TEST_CASE("a new instance joins the session the user was most recently active in") {
    TestDirectory t;
    LinkDirectory project(t.name), inserted(t.name);  // two mappings: two processes, in effect
    REQUIRE(project.open());
    REQUIRE(inserted.open());
    const Uuid a = Uuid::generate(), b = Uuid::generate(), fresh = Uuid::generate();

    project.touch(a, kT0);
    project.touch(b, kT0 + 1000);
    CHECK(sessionForNewInstance(inserted, kT0 + 2000, fresh) == b);

    project.touch(a, kT0 + 3000);
    CHECK(sessionForNewInstance(inserted, kT0 + 4000, fresh) == a);
}

TEST_CASE("a heartbeat keeps a session listed but never makes it the most recent") {
    //  A background project that is merely playing refreshes all the time. If that counted as
    //  activity, a new instance would join whichever project refreshed last.
    TestDirectory t;
    LinkDirectory dir(t.name);
    REQUIRE(dir.open());
    const Uuid background = Uuid::generate(), working = Uuid::generate();

    dir.touch(background, kT0);
    dir.touch(working, kT0 + 1000);
    for (int i = 1; i <= 100; ++i) dir.heartbeat(background, kT0 + 1000 + static_cast<std::uint64_t>(i) * 10'000);

    const std::uint64_t now = kT0 + 1'100'000;
    CHECK(dir.mostRecent(now) == working);
    std::vector<LinkSessionInfo> live;
    dir.list(live, now);
    CHECK(live.size() == 2);
}

TEST_CASE("with no live session, a new instance starts its own -- also without shared memory") {
    TestDirectory t;
    const Uuid fresh = Uuid::generate(), other = Uuid::generate();

    LinkDirectory dir(t.name);
    REQUIRE(dir.open());
    CHECK(sessionForNewInstance(dir, kT0, fresh) == fresh);

    LinkDirectory unavailable(t.name);  // never opened: what a failed open leaves
    unavailable.touch(other, kT0);
    unavailable.heartbeat(other, kT0);
    unavailable.remove(other);
    CHECK_FALSE(unavailable.isOpen());
    CHECK(sessionForNewInstance(unavailable, kT0, fresh) == fresh);
    CHECK_FALSE(dir.mostRecent(kT0).has_value());  // and it wrote nothing through the no-ops
}

TEST_CASE("a session whose instances all went quiet lapses, and is never joined") {
    TestDirectory t;
    LinkDirectory dir(t.name);
    REQUIRE(dir.open());
    const Uuid alive = Uuid::generate(), gone = Uuid::generate();

    dir.touch(alive, kT0);
    dir.touch(gone, kT0 + 1000);                          // more recent...
    dir.heartbeat(alive, kT0 + kLinkHeartbeatTimeoutUs);  // ...but only `alive` keeps refreshing

    const std::uint64_t now = kT0 + kLinkHeartbeatTimeoutUs + 2000;
    CHECK(dir.mostRecent(now) == alive);
    std::vector<LinkSessionInfo> live;
    dir.list(live, now);
    CHECK_FALSE(lists(live, gone));
}

TEST_CASE("a project that closes is unlisted at once, so the next new project does not join it") {
    TestDirectory t;
    LinkDirectory dir(t.name);
    REQUIRE(dir.open());
    const Uuid open = Uuid::generate(), closed = Uuid::generate();

    dir.touch(open, kT0);
    dir.touch(closed, kT0 + 1000);
    dir.remove(closed);
    CHECK(dir.mostRecent(kT0 + 2000) == open);
}

TEST_CASE("one session listed from many processes at once is one entry, with its latest activity") {
    //  Eight instances of one session list it at the same instant, many times over. Nothing stops
    //  two of them claiming an entry each -- a lock across processes would be worse -- so list()
    //  must show one session however many entries it has. The start signal is what makes the
    //  claims collide; without it the first thread lists the session before the rest arrive, and
    //  this test could not tell a merge from its absence.
    int rounds = 0;
    for (int round = 0; round < 100; ++round) {
        TestDirectory t;
        //  Held open across the round, as a live session's instances hold it: on Windows a directory nothing has
        //  open is gone, and the reader below would find a fresh, empty one.
        LinkDirectory dir(t.name);
        REQUIRE(dir.open());
        const Uuid s = Uuid::generate();
        std::atomic<bool> go{false};
        std::atomic<int> opened{0};
        std::vector<std::thread> threads;
        for (int k = 0; k < 8; ++k)
            threads.emplace_back([&, k] {
                LinkDirectory d(t.name);
                const bool ok = d.open();
                opened.fetch_add(ok ? 1 : 100);
                while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                if (ok) d.touch(s, kT0 + static_cast<std::uint64_t>(k));
            });
        while (opened.load(std::memory_order_acquire) < 8) std::this_thread::yield();
        go.store(true, std::memory_order_release);
        for (auto& th : threads) th.join();
        REQUIRE(opened.load() == 8);

        std::vector<LinkSessionInfo> live;
        dir.list(live, kT0 + 100);
        REQUIRE(live.size() == 1);
        CHECK(live[0].session == s);
        CHECK(live[0].lastActivity == kT0 + 7);
        ++rounds;
    }
    CHECK(rounds == 100);
}

TEST_CASE("a full directory refuses a new session rather than evicting a live one") {
    TestDirectory t;
    LinkDirectory dir(t.name);
    REQUIRE(dir.open());
    std::vector<Uuid> ids(kLinkDirectorySlots);
    for (auto& id : ids) {
        id = Uuid::generate();
        dir.touch(id, kT0);
    }
    const Uuid extra = Uuid::generate();
    dir.touch(extra, kT0 + 1000);

    std::vector<LinkSessionInfo> live;
    dir.list(live, kT0 + 2000);
    CHECK(live.size() == static_cast<std::size_t>(kLinkDirectorySlots));
    CHECK_FALSE(lists(live, extra));

    //  Once one lapses there is room.
    for (std::size_t i = 1; i < ids.size(); ++i) dir.heartbeat(ids[i], kT0 + 1'500'000);
    dir.touch(extra, kT0 + 2'500'000);
    dir.list(live, kT0 + 2'500'001);
    CHECK(live.size() == static_cast<std::size_t>(kLinkDirectorySlots));
    CHECK(lists(live, extra));
    CHECK_FALSE(lists(live, ids[0]));
    CHECK(dir.mostRecent(kT0 + 2'500'001) == extra);
}

TEST_CASE("a listed session id is never torn, however entries churn") {
    TestDirectory t;
    //  Every byte of each id is the same, so any mix of two ids is detectably not one of them.
    //  A smoke test for the claim protocol under churn, not proof of the re-check in read(): a
    //  16-byte copy may well be a single load on this hardware, so a torn id may be unobservable
    //  here. The re-check is argued where it is written (directory.cpp).
    std::array<Uuid, 16> known{};
    for (std::size_t k = 0; k < known.size(); ++k) known[k].bytes.fill(static_cast<std::uint8_t>(0x10 + k));

    std::atomic<bool> stop{false};
    std::atomic<long> seen{0}, torn{0};
    std::atomic<int> opened{0};
    std::vector<std::thread> writers;
    for (int w = 0; w < 4; ++w)
        writers.emplace_back([&, w] {
            LinkDirectory d(t.name);
            if (!d.open()) return;
            opened.fetch_add(1);
            std::uint64_t now = kT0;
            for (int i = 0; !stop.load(std::memory_order_relaxed); ++i, ++now) {
                const Uuid& s = known[static_cast<std::size_t>((w * 5 + i) % 16)];
                d.touch(s, now);
                if (i % 3 == 0) d.remove(s);
            }
        });
    std::thread reader([&] {
        LinkDirectory d(t.name);
        if (!d.open()) return;
        opened.fetch_add(1);
        std::vector<LinkSessionInfo> live;
        while (!stop.load(std::memory_order_relaxed)) {
            d.list(live, kT0 + 1000);
            for (const auto& s : live) {
                seen.fetch_add(1);
                if (std::none_of(known.begin(), known.end(), [&](const Uuid& k) { return k == s.session; }))
                    torn.fetch_add(1);
            }
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop = true;
    for (auto& w : writers) w.join();
    reader.join();

    REQUIRE(opened.load() == 5);
    CHECK(seen.load() > 1000);  // not vacuous: the reader really saw listed sessions
    CHECK(torn.load() == 0);
}
