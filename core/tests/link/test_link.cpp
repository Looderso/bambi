// SPDX-License-Identifier: GPL-3.0-or-later
//
//  The link bus is the one module where "it passed" and "it is correct" come apart most
//  easily: a data race that never fires on an idle test machine still corrupts a user's
//  scene. So both seqlocks are tested with real threads and self-checking payloads rather
//  than by inspection. What only separate processes can show lives in tools/link-test.sh.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <limits>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "../../src/link/os.hpp"
#include "bambi/echo/params.hpp"
#include "bambi/encode/params.hpp"
#include "bambi/link/link.hpp"
#include "bambi/mod/matrix.hpp"
#include "bambi/mod/sources.hpp"
#include "bambi/path/generator.hpp"
#include "doctest.h"

using namespace bambi;

namespace {

//  The concurrency tests read until they have this many samples, not for a fixed time, so a loaded
//  machine does not fail them; the deadline only bounds a hang.
constexpr int kEnoughReads = 1000;
constexpr auto kReadDeadline = std::chrono::seconds(5);

/*  A writer's pause between bursts: until the reader has taken one more, or `most` has passed. Tied to the reader
    rather than to a duration, so neither the system's sleep granularity (15.6 ms on Windows) nor a machine with
    fewer cores than threads starves the reader; it sleeps while it waits, and leaves the reader the core. */
void pauseForReader(const std::atomic<int>& reads, std::chrono::steady_clock::duration most) {
    const int seen = reads.load(std::memory_order_relaxed);
    const auto until = std::chrono::steady_clock::now() + most;
    while (reads.load(std::memory_order_relaxed) == seen && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::microseconds(100));
}

/*  A session nobody else is using. Tests run in one process and share the machine's shared
 *  memory namespace, so a fixed name would collide with a previous failed run. */
struct Session {
    Uuid id{Uuid::generate()};
    ~Session() { LinkBus::unlinkSession(id); }
};

Trajectory buildPath(GeneratorType g) {
    TrajectoryState ts;
    ts.kind = TrajectoryKind::Parametric;
    ts.generator = g;
    generatorDefaults(g, ts.genParams);
    Trajectory t;
    t.build(ts);
    return t;
}

/*  Every field is a fixed multiple of one counter, so "did I get one write or halves of two"
 *  is an exact question. Floats are not exactly representable, so a payload built from cos/sin
 *  would report tears that are really just the float round-trip.
 *  When a test asks whether two things came from the same write, its payload must be exact. */
LinkDynamic dynAt(int n) {
    LinkDynamic d;
    d.s = static_cast<float>(n);
    d.x = static_cast<float>(2 * n);
    d.y = static_cast<float>(3 * n);
    d.z = static_cast<float>(4 * n);
    d.live[0] = static_cast<float>(5 * n);  // the engine's values, first and last: the record's far end too
    d.live[kMaxParams - 1] = static_cast<float>(5 * n);
    d.level = static_cast<float>(6 * n);
    d.yawRad = static_cast<float>(7 * n);
    d.extent = static_cast<float>(8 * n);
    d.muted = static_cast<std::uint8_t>(n & 1);
    return d;
}

bool consistent(const LinkDynamic& d) {
    const float n = d.s;
    return d.x == 2 * n && d.y == 3 * n && d.z == 4 * n && d.live[0] == 5 * n && d.live[kMaxParams - 1] == 5 * n &&
           d.level == 6 * n && d.yawRad == 7 * n && d.extent == 8 * n && d.muted == (static_cast<int>(n) & 1);
}

LinkStatic staticAt(int n) {
    LinkStatic st;
    st.setLabel("path-" + std::to_string(n));
    st.colour = static_cast<std::uint8_t>(n & 0xFF);
    st.order = static_cast<std::uint8_t>(n & 0x07);
    st.closed = static_cast<std::uint8_t>(n & 1);
    st.pointCount = kLinkPathPoints;
    st.centre = {static_cast<float>(n), static_cast<float>(n), static_cast<float>(n)};
    for (auto& p : st.path) p = {static_cast<float>(n), static_cast<float>(2 * n), static_cast<float>(3 * n)};
    return st;
}

bool consistent(const LinkStatic& st) {
    const float n = st.path[0].x;
    const int k = static_cast<int>(n);
    if (st.labelString() != "path-" + std::to_string(k)) return false;
    if (st.colour != (k & 0xFF) || st.order != (k & 0x07) || st.closed != (k & 1)) return false;
    if (st.centre.x != n || st.centre.y != n || st.centre.z != n) return false;
    for (const auto& p : st.path)
        if (p.x != n || p.y != 2 * n || p.z != 3 * n) return false;
    return true;
}

}  // namespace

// ------------------------------------------------------------------------- observation

TEST_CASE("two instances see each other's paths and positions") {
    Session s;
    const Uuid idA = Uuid::generate(), idB = Uuid::generate();
    LinkBus a, b;
    REQUIRE(a.open(s.id, idA, Product::Encoder));
    REQUIRE(b.open(s.id, idB, Product::Encoder));

    const Trajectory orbit = buildPath(GeneratorType::Orbit);
    LinkStatic sa;
    sa.setLabel("kick");
    sa.setPath(orbit);
    a.publishStatic(sa);

    LinkDynamic da;
    da.s = 0.25f;
    const Vec3 p = orbit.eval(0.25);
    da.x = static_cast<float>(p.x);
    da.y = static_cast<float>(p.y);
    da.z = static_cast<float>(p.z);
    a.publishDynamic(da);

    LinkScene scene;
    scene.update(b);
    REQUIRE(scene.entries().size() == 2);  // the viewer sees itself too

    const auto* e = scene.find(idA);
    REQUIRE(e != nullptr);
    CHECK(e->st.labelString() == "kick");
    CHECK(e->st.pointCount == static_cast<std::uint32_t>(linkPathPointsFor(orbit.lengthRad())));
    CHECK(e->dyn.s == doctest::Approx(0.25f));
    CHECK(e->hasPosition);

    //  b joined but never published where it is: visible, with nothing to draw a dot for.
    REQUIRE(scene.find(idB) != nullptr);
    CHECK_FALSE(scene.find(idB)->hasPosition);

    //  The receiver can place the source on the path it was given: on a closed path, s = 0.25 is the point a
    //  quarter of the way through, and it agrees with the position the sender resolved.
    REQUIRE(e->st.pointCount % 4 == 0);
    CHECK(sameDir(e->st.point(static_cast<int>(e->st.pointCount / 4)), e->dyn.position(), 1e-5));
}

/*  Two projects open at once must not see each other's sources. A host process id could not
 *  give this, because hosts sandbox plugins across several processes -- which is the whole
 *  reason identity carries a session uuid. */
TEST_CASE("different sessions are invisible to each other") {
    Session s1, s2;
    LinkBus a, b;
    REQUIRE(a.open(s1.id, Uuid::generate(), Product::Encoder));
    REQUIRE(b.open(s2.id, Uuid::generate(), Product::Encoder));

    std::vector<LinkPeer> seen;
    a.poll(seen);
    CHECK(seen.size() == 1);
    b.poll(seen);
    CHECK(seen.size() == 1);
}

TEST_CASE("a reader never sees half of one position and half of another") {
    Session s;
    const Uuid writerId = Uuid::generate();
    LinkBus writer, reader;
    REQUIRE(writer.open(s.id, writerId, Product::Encoder));
    REQUIRE(reader.open(s.id, Uuid::generate(), Product::Encoder));
    writer.publishDynamic(dynAt(1));

    std::atomic<bool> stop{false};
    std::thread w([&] {
        int n = 1;
        while (!stop.load(std::memory_order_relaxed)) {
            writer.publishDynamic(dynAt(n));
            if (++n > 100000) n = 1;
        }
    });

    int reads = 0, torn = 0;
    std::vector<LinkPeer> peers;
    const auto until = std::chrono::steady_clock::now() + kReadDeadline;
    while (reads < kEnoughReads && std::chrono::steady_clock::now() < until) {
        reader.poll(peers);
        for (const auto& p : peers) {
            //  Filter by identity, not by value: a default section is a perfectly plausible
            //  value, and filtering on one once counted the reader's own slot as torn.
            if (!(p.instance == writerId)) continue;
            ++reads;
            if (!consistent(p.dyn)) ++torn;
        }
    }
    stop.store(true);
    w.join();

    INFO("checked ", reads, " positions");
    CHECK(reads >= kEnoughReads);
    CHECK(torn == 0);
}

/*  The generation is what lets a scene skip copying 64 unchanged paths every frame, so it has
 *  to be trustworthy: a reader must never pair a new path with an old generation number that
 *  it will then cache forever, or an old path with a new one. The writer publishes path n as
 *  its n-th publish on a fresh slot, so content and generation must be equal.
 *
 *  What this does not catch: moving the generation write one line outside the seqlock still
 *  passes here. The window that opens is a single instruction wide, and the reader copies the
 *  generation after a path of up to 49 KB, so no timing test will land in it. That rule is
 *  guarded by the memory model and by the comment at the store in writeStatic; this test guards
 *  everything wider. */
TEST_CASE("a path and its generation always arrive together") {
    Session s;
    const Uuid writerId = Uuid::generate();
    LinkBus writer, reader;
    REQUIRE(writer.open(s.id, writerId, Product::Encoder));
    REQUIRE(reader.open(s.id, Uuid::generate(), Product::Encoder));

    std::atomic<bool> stop{false};
    std::thread w([&] {
        for (int n = 1; n <= 2'000'000 && !stop.load(std::memory_order_relaxed); ++n) writer.publishStatic(staticAt(n));
    });

    int reads = 0, torn = 0, mismatched = 0, backwards = 0;
    std::uint32_t lastGen = 0;
    std::vector<LinkPeer> peers;
    LinkStatic st;
    const auto until = std::chrono::steady_clock::now() + kReadDeadline;
    while (reads < kEnoughReads && std::chrono::steady_clock::now() < until) {
        reader.poll(peers);
        for (const auto& p : peers) {
            if (!(p.instance == writerId)) continue;
            std::uint32_t gen = 0;
            if (!reader.readStatic(p, st, gen)) continue;
            ++reads;
            if (!consistent(st)) ++torn;
            if (st.path[0].x != static_cast<float>(gen)) ++mismatched;
            if (gen < lastGen) ++backwards;
            lastGen = gen;
        }
    }
    stop.store(true);
    w.join();

    INFO("checked ", reads, " paths, last generation ", lastGen);
    CHECK(reads >= kEnoughReads);
    CHECK(torn == 0);
    CHECK(mismatched == 0);
    CHECK(backwards == 0);
}

TEST_CASE("the scene copies a path again only when its generation moves") {
    Session s;
    const Uuid idA = Uuid::generate();
    LinkBus a, viewer;
    REQUIRE(a.open(s.id, idA, Product::Encoder));
    REQUIRE(viewer.open(s.id, Uuid::generate(), Product::Encoder));

    a.publishStatic(staticAt(7));
    LinkScene scene;
    for (int i = 0; i < 20; ++i) scene.update(viewer);
    CHECK(scene.staticReads() == 1);  // twenty frames, one copy
    REQUIRE(scene.find(idA) != nullptr);
    CHECK(scene.find(idA)->st.path[0].x == 7.0f);

    a.publishStatic(staticAt(9));
    scene.update(viewer);
    scene.update(viewer);
    CHECK(scene.staticReads() == 2);
    CHECK(scene.find(idA)->st.path[0].x == 9.0f);
    CHECK(scene.find(idA)->generation == 2);
}

TEST_CASE("a peer that leaves also leaves the scene") {
    Session s;
    LinkBus viewer;
    REQUIRE(viewer.open(s.id, Uuid::generate(), Product::Encoder));
    LinkScene scene;

    const Uuid idA = Uuid::generate();
    {
        LinkBus a;
        REQUIRE(a.open(s.id, idA, Product::Encoder));
        a.publishStatic(staticAt(1));
        scene.update(viewer);
        REQUIRE(scene.find(idA) != nullptr);
    }
    //  Closing releases the slot rather than waiting out the heartbeat: a plugin the user
    //  deleted should leave the scene immediately, not in two seconds.
    scene.update(viewer);
    CHECK(scene.find(idA) == nullptr);
    CHECK(scene.entries().size() == 1);  // just the viewer
    CHECK(viewer.slotCount() == 1);
}

TEST_CASE("a path is never read back under the wrong instance's name") {
    Session s;
    LinkBus viewer;
    REQUIRE(viewer.open(s.id, Uuid::generate(), Product::Encoder));

    const Uuid idA = Uuid::generate();
    LinkPeer peerA;
    {
        LinkBus a;
        REQUIRE(a.open(s.id, idA, Product::Encoder));
        a.publishStatic(staticAt(1));
        std::vector<LinkPeer> peers;
        viewer.poll(peers);
        for (const auto& p : peers)
            if (p.instance == idA) peerA = p;
        REQUIRE(peerA.slot >= 0);
    }

    LinkBus c;
    REQUIRE(c.open(s.id, Uuid::generate(), Product::Encoder));
    REQUIRE(c.selfSlot() == peerA.slot);  // the newcomer really did inherit the slot
    c.publishStatic(staticAt(2));

    LinkStatic st;
    std::uint32_t gen = 0;
    CHECK_FALSE(viewer.readStatic(peerA, st, gen));
}

TEST_CASE("a path is sampled at equal steps of arc length, untransformed") {
    for (GeneratorType g : {GeneratorType::Orbit, GeneratorType::Arc}) {
        const Trajectory t = buildPath(g);
        LinkStatic st;
        st.setPath(t);
        INFO("generator ", static_cast<int>(g));

        const int count = linkPathPointsFor(t.lengthRad());
        REQUIRE(st.pointCount == static_cast<std::uint32_t>(count));
        CHECK((st.closed != 0) == t.closed());

        //  A closed path spans [0, 1) so its loop is implied; an open one spans [0, 1] so it
        //  keeps both ends.
        const double denom = t.closed() ? count : count - 1;
        for (int i = 0; i < count; ++i) CHECK(sameDir(st.point(i), t.eval(i / denom), 1e-6));
        CHECK(sameDir(st.centreVec(), pathCentre(t.points()), 1e-6));
    }
    CHECK_FALSE(buildPath(GeneratorType::Arc).closed());  // so both branches really ran
}

TEST_CASE("the dynamic section carries a transform the receiver can apply") {
    LinkDynamic d;
    d.yawRad = 0.5f;
    d.pitchRad = -0.25f;
    d.rollRad = 0.75f;
    d.extent = 0.5f;
    const PathTransform t = d.transform();
    CHECK(t.yawRad == doctest::Approx(0.5));
    CHECK(t.pitchRad == doctest::Approx(-0.25));
    CHECK(t.rollRad == doctest::Approx(0.75));
    CHECK(t.extent == doctest::Approx(0.5));
}

// ----------------------------------------------------------------------------- publisher

TEST_CASE("the publisher keeps an instance visible, and in place, when audio stops") {
    Session s;
    const Uuid id = Uuid::generate();
    LinkBus bus, viewer;
    REQUIRE(bus.open(s.id, id, Product::Encoder));
    REQUIRE(viewer.open(s.id, Uuid::generate(), Product::Encoder));

    const auto sOf = [&] {
        std::vector<LinkPeer> peers;
        viewer.poll(peers);
        for (const auto& p : peers)
            if (p.instance == id) return p.dyn.s;
        return -1.0f;
    };

    LinkPublisher pub(bus);
    LinkDynamic seed;
    seed.s = 0.7f;
    pub.seed(seed);
    pub.tick();
    CHECK(sOf() == doctest::Approx(0.7f));  // not the default front-facing position

    LinkDynamic d;
    for (float v : {0.1f, 0.2f, 0.3f}) {
        d.s = v;
        pub.submit(d);
    }
    pub.tick();
    CHECK(sOf() == doctest::Approx(0.3f));  // the newest, not the first or the seed

    //  Audio stops submitting. Ticks continue and must keep publishing the last position --
    //  not nothing, which would let the heartbeat lapse, and not a default, which would make
    //  the source jump.
    for (int i = 0; i < 5; ++i) pub.tick();
    CHECK(sOf() == doctest::Approx(0.3f));
}

TEST_CASE("audio thread to scene, end to end, under contention") {
    Session s;
    const Uuid id = Uuid::generate();
    LinkBus bus, viewer;
    REQUIRE(bus.open(s.id, id, Product::Encoder));
    REQUIRE(viewer.open(s.id, Uuid::generate(), Product::Encoder));

    LinkPublisher pub(bus);
    pub.seed(dynAt(1));  // before either thread starts, so the first tick is consistent too

    std::atomic<bool> stop{false};
    std::thread audio([&] {
        int n = 1;
        while (!stop.load(std::memory_order_relaxed)) {
            pub.submit(dynAt(n));
            if (++n > 100000) n = 1;
        }
    });
    std::thread timer([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            pub.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    int reads = 0, torn = 0;
    std::vector<LinkPeer> peers;
    const auto until = std::chrono::steady_clock::now() + kReadDeadline;
    while (reads < kEnoughReads && std::chrono::steady_clock::now() < until) {
        viewer.poll(peers);
        for (const auto& p : peers) {
            if (!(p.instance == id)) continue;
            //  Before the timer's first tick the slot is visible but holds join()'s default,
            //  not a position. This test found exactly that -- 81 "torn" reads that were the
            //  default -- and skipping it is what a scene must do, not a test convenience.
            if (!p.hasPosition) continue;
            ++reads;
            if (!consistent(p.dyn)) ++torn;
        }
    }
    stop.store(true);
    audio.join();
    timer.join();

    INFO("checked ", reads, " positions");
    CHECK(reads >= kEnoughReads);
    CHECK(torn == 0);
}

// ------------------------------------------------------------------------------ commands

TEST_CASE("a command reaches its target, exactly once, stamped with the sender") {
    Session s;
    const Uuid idA = Uuid::generate(), idB = Uuid::generate();
    LinkBus a, b;
    REQUIRE(a.open(s.id, idA, Product::Encoder));
    REQUIRE(b.open(s.id, idB, Product::Encoder));

    LinkCommand c;
    c.param = static_cast<std::uint32_t>(EncoderParam::RenderWidth);
    c.value = 42.0f;
    c.gesture = 1;
    REQUIRE(a.sendCommand(idB, c));

    std::vector<LinkCommand> got;
    a.drainCommands(got);
    CHECK(got.empty());  // not delivered to the sender

    b.drainCommands(got);
    REQUIRE(got.size() == 1);
    CHECK(got[0].paramId() == EncoderParam::RenderWidth);
    CHECK(got[0].value == doctest::Approx(42.0f));
    CHECK(got[0].gesture == 1);
    CHECK(got[0].from == idA);  // stamped by the bus, not by the caller

    b.drainCommands(got);
    CHECK(got.empty());  // and consumed, not redelivered
}

TEST_CASE("commands to a departed instance fail rather than vanish silently") {
    Session s;
    const Uuid idB = Uuid::generate();
    LinkBus a;
    REQUIRE(a.open(s.id, Uuid::generate(), Product::Encoder));
    {
        LinkBus b;
        REQUIRE(b.open(s.id, idB, Product::Encoder));
        CHECK(a.sendCommand(idB, LinkCommand{}));
    }
    CHECK_FALSE(a.sendCommand(idB, LinkCommand{}));  // gone: the caller is told
}

/*  A full inbox refuses. Whatever the sender then decides, the refusal must be clean: it may
 *  not overwrite unread commands or corrupt the ring. */
TEST_CASE("a full inbox refuses without corrupting what is already queued") {
    Session s;
    const Uuid idB = Uuid::generate();
    LinkBus a, b;
    REQUIRE(a.open(s.id, Uuid::generate(), Product::Encoder));
    REQUIRE(b.open(s.id, idB, Product::Encoder));

    int accepted = 0;
    for (int i = 0; i < kLinkInboxSlots * 3; ++i) {
        LinkCommand c;
        c.param = static_cast<std::uint32_t>(EncoderParam::RenderWidth);
        c.value = static_cast<float>(i);
        if (a.sendCommand(idB, c)) ++accepted;
    }
    const int valueCapacity = kLinkInboxSlots - kLinkInboxReserved;  // the rest is for endpoints
    CHECK(accepted == valueCapacity);

    std::vector<LinkCommand> got;
    b.drainCommands(got);
    REQUIRE(got.size() == static_cast<std::size_t>(valueCapacity));
    //  The queued ones are the first n, in order, undamaged by the attempts that failed.
    for (int i = 0; i < valueCapacity; ++i)
        CHECK(got[static_cast<std::size_t>(i)].value == doctest::Approx(static_cast<float>(i)));
}

/*  Catches producers claiming an index by CAS and then writing the payload, which lets a
 *  consumer read a cell that was claimed but not yet filled and hand back the previous
 *  generation's command -- it surfaces as duplicates.
 *
 *  What it does not reliably catch is a narrow reordering of the fixed protocol -- storing
 *  the ready flag one line before the payload instead of after still passes here, because
 *  the window is a few nanoseconds wide. That ordering is justified by the memory model
 *  rather than by this test, and it is written down at the store so it is not "tidied". */
TEST_CASE("many senders can command one receiver without losing or duplicating") {
    Session s;
    const Uuid target = Uuid::generate();
    LinkBus receiver;
    REQUIRE(receiver.open(s.id, target, Product::Encoder));

    constexpr int kSenders = 4;
    constexpr int kEach = 200;
    std::vector<std::unique_ptr<LinkBus>> senders;
    for (int i = 0; i < kSenders; ++i) {
        auto bus = std::make_unique<LinkBus>();
        REQUIRE(bus->open(s.id, Uuid::generate(), Product::Encoder));
        senders.push_back(std::move(bus));
    }

    std::atomic<int> sent{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int i = 0; i < kSenders; ++i) {
        threads.emplace_back([&, i] {
            while (!go.load()) {}
            for (int k = 0; k < kEach; ++k) {
                LinkCommand c;
                c.param = static_cast<std::uint32_t>(EncoderParam::RenderWidth);
                c.value = static_cast<float>(i * 1000 + k);
                //  Retry on a full inbox so the count is exact.
                while (!senders[static_cast<std::size_t>(i)]->sendCommand(target, c)) std::this_thread::yield();
                ++sent;
            }
        });
    }

    std::vector<LinkCommand> all, batch;
    go.store(true);
    while (static_cast<int>(all.size()) < kSenders * kEach) {
        receiver.drainCommands(batch);
        for (const auto& c : batch) all.push_back(c);
    }
    for (auto& t : threads) t.join();
    receiver.drainCommands(batch);
    for (const auto& c : batch) all.push_back(c);

    CHECK(sent.load() == kSenders * kEach);
    REQUIRE(all.size() == static_cast<std::size_t>(kSenders * kEach));

    //  Every value exactly once: nothing lost to a CAS race, nothing delivered twice.
    std::vector<int> seen(static_cast<std::size_t>(kSenders * 1000 + kEach), 0);
    for (const auto& c : all) ++seen[static_cast<std::size_t>(c.value)];
    for (int i = 0; i < kSenders; ++i)
        for (int k = 0; k < kEach; ++k) REQUIRE(seen[static_cast<std::size_t>(i * 1000 + k)] == 1);
}

// --------------------------------------------------------------------------- boundaries

TEST_CASE("the bus is bounded, and says so instead of failing obscurely") {
    Session s;
    std::vector<std::unique_ptr<LinkBus>> buses;
    for (int i = 0; i < kLinkMaxInstances; ++i) {
        auto b = std::make_unique<LinkBus>();
        REQUIRE(b->open(s.id, Uuid::generate(), Product::Encoder));
        buses.push_back(std::move(b));
    }
    LinkBus overflow;
    CHECK_FALSE(overflow.open(s.id, Uuid::generate(), Product::Encoder));
    CHECK(overflow.error().find("no free slot") != std::string::npos);
}

TEST_CASE("a closed bus is safe to use — the plugin must survive shared memory being absent") {
    LinkBus dead;
    CHECK_FALSE(dead.isOpen());
    dead.publishStatic(LinkStatic{});
    dead.publishDynamic(LinkDynamic{});

    std::vector<LinkPeer> peers;
    dead.poll(peers);
    CHECK(peers.empty());

    LinkStatic st;
    std::uint32_t gen = 0;
    CHECK_FALSE(dead.readStatic(LinkPeer{}, st, gen));

    std::vector<LinkCommand> cmds;
    dead.drainCommands(cmds);
    CHECK(cmds.empty());
    CHECK_FALSE(dead.sendCommand(Uuid::generate(), LinkCommand{}));
    CHECK(dead.slotCount() == 0);

    LinkPublisher pub(dead);
    pub.submit(LinkDynamic{});
    pub.tick();
    LinkScene scene;
    scene.update(dead);
    CHECK(scene.entries().empty());
}

TEST_CASE("a command naming a parameter this build lacks resolves to Count, not to garbage") {
    LinkCommand c;
    c.param = 999999;
    CHECK(c.paramId() == kNoParamId);
}

/*  The bug this pins made a live source vanish from the scene for one frame, about once
 *  every eighty, at random. It looks exactly like a race in the seqlock and is not one --
 *  it is an unsigned subtraction. Cheap to assert, invisible otherwise. */
TEST_CASE("a heartbeat from the future is fresh, not 584000 years stale") {
    const std::uint64_t now = 1'000'000'000;

    CHECK(linkHeartbeatAlive(now, now));
    CHECK(linkHeartbeatAlive(now - 1000, now));                     // just published
    CHECK(linkHeartbeatAlive(now - kLinkHeartbeatTimeoutUs, now));  // exactly at the limit
    CHECK_FALSE(linkHeartbeatAlive(now - kLinkHeartbeatTimeoutUs - 1, now));
    CHECK_FALSE(linkHeartbeatAlive(0, now));  // never published

    //  `now` is sampled once before the slot walk, so a peer publishing during the walk is
    //  ahead of it. Unguarded, now - hb wraps to ~1.8e19 and the peer is dropped.
    CHECK(linkHeartbeatAlive(now + 1, now));
    CHECK(linkHeartbeatAlive(now + 500'000, now));
}

// ------------------------------------------------------------------------- remote edits

namespace {

LinkCommand edit(const Uuid& from, ParamId p, std::uint32_t gesture, float value) {
    LinkCommand c;
    c.from = from;
    c.param = static_cast<std::uint32_t>(p);
    c.gesture = gesture;
    c.value = value;
    return c;
}

/// "B V10 V20 E" -- what the host is told, readable in a failure message.
std::string describe(const std::vector<LinkEditAction>& actions) {
    std::string s;
    char buf[32];
    for (const auto& a : actions) {
        if (!s.empty()) s += ' ';
        switch (a.kind) {
            case LinkEditAction::Kind::Begin: s += 'B'; break;
            case LinkEditAction::Kind::Value:
                std::snprintf(buf, sizeof buf, "V%g", static_cast<double>(a.value));
                s += buf;
                break;
            case LinkEditAction::Kind::End: s += a.abandoned ? "E!" : "E"; break;
        }
    }
    return s;
}

}  // namespace

TEST_CASE("a gesture's endpoints still fit when values have filled the inbox") {
    Session s;
    const Uuid idB = Uuid::generate();
    LinkBus a, b;
    REQUIRE(a.open(s.id, Uuid::generate(), Product::Encoder));
    REQUIRE(b.open(s.id, idB, Product::Encoder));

    LinkCommand c = edit(Uuid{}, EncoderParam::RenderWidth, kLinkGestureBegin, 10.0f);
    REQUIRE(a.sendCommand(idB, c));

    //  A long drag against a stalled receiver: values until they are refused.
    c.gesture = kLinkGestureNone;
    int values = 0;
    for (int i = 0; i < kLinkInboxSlots * 2; ++i) {
        c.value = 11.0f + static_cast<float>(i);
        if (a.sendCommand(idB, c)) ++values;
    }
    CHECK(values == kLinkInboxSlots - kLinkInboxReserved - 1);  // the begin took one

    //  The release still fits, carrying the value the user let go at.
    c.gesture = kLinkGestureEnd;
    c.value = 99.0f;
    REQUIRE(a.sendCommand(idB, c));

    std::vector<LinkCommand> got;
    b.drainCommands(got);
    REQUIRE(got.size() == static_cast<std::size_t>(values + 2));
    CHECK(got.front().gesture == kLinkGestureBegin);
    CHECK(got.back().gesture == kLinkGestureEnd);
    CHECK(got.back().value == doctest::Approx(99.0f));

    //  And the reserve is a reserve, not a bypass: endpoints are bounded too.
    int endpoints = 0;
    for (int i = 0; i < kLinkInboxSlots * 2; ++i)
        if (a.sendCommand(idB, c)) ++endpoints;
    CHECK(endpoints == kLinkInboxSlots);
}

TEST_CASE("a remote gesture becomes one well-formed host gesture") {
    const Uuid a = Uuid::generate();
    LinkEditReceiver rx;
    std::vector<LinkEditAction> out;
    rx.apply(std::vector{edit(a, EncoderParam::RenderWidth, kLinkGestureBegin, 10),
                         edit(a, EncoderParam::RenderWidth, kLinkGestureNone, 20),
                         edit(a, EncoderParam::RenderWidth, kLinkGestureEnd, 30)},
             out);
    CHECK(describe(out) == "B V10 V20 V30 E");
    CHECK(rx.openGestures() == 0);
}

/*  Catches the index being used unchecked: `paramId` passes anything up to 0xFFFF and the receiver
 *  keeps a gesture per parameter up to the cap, so an index past it wrote outside the array -- from
 *  a command another process put in shared memory. Under ASan the mutation is a crash; without it,
 *  the actions it emits for a parameter nobody has are what shows. */
TEST_CASE("a command for a parameter past the cap is dropped, not indexed") {
    const Uuid a = Uuid::generate();
    LinkEditReceiver rx;
    std::vector<LinkEditAction> out;
    rx.apply(std::vector{edit(a, static_cast<ParamId>(kMaxParams), kLinkGestureBegin, 1),
                         edit(a, static_cast<ParamId>(60000), kLinkGestureNone, 2)},
             out);
    CHECK(out.empty());
    CHECK(rx.openGestures() == 0);
}

TEST_CASE("the release value lands even when every value in between was lost") {
    const Uuid a = Uuid::generate();
    LinkEditReceiver rx;
    std::vector<LinkEditAction> out;
    rx.apply(std::vector{edit(a, EncoderParam::RenderWidth, kLinkGestureBegin, 10),
                         edit(a, EncoderParam::RenderWidth, kLinkGestureEnd, 99)},
             out);
    CHECK(describe(out) == "B V10 V99 E");
}

TEST_CASE("a change outside any gesture is applied as a complete one") {
    const Uuid a = Uuid::generate();
    LinkEditReceiver rx;
    std::vector<LinkEditAction> out;

    rx.apply(std::vector{edit(a, EncoderParam::RenderWidth, kLinkGestureNone, 5)}, out);
    CHECK(describe(out) == "B V5 E");

    //  An end whose begin never arrived still lands its value.
    out.clear();
    rx.apply(std::vector{edit(a, EncoderParam::RenderWidth, kLinkGestureEnd, 7)}, out);
    CHECK(describe(out) == "B V7 E");
    CHECK(rx.openGestures() == 0);
}

TEST_CASE("last begin wins: a second window takes over and the first is ignored") {
    const Uuid a = Uuid::generate(), b = Uuid::generate();
    const ParamId w = EncoderParam::RenderWidth;
    LinkEditReceiver rx;
    std::vector<LinkEditAction> out;
    rx.apply(std::vector{edit(a, w, kLinkGestureBegin, 1), edit(b, w, kLinkGestureBegin, 2),  // takes over
                         edit(a, w, kLinkGestureNone, 3),                                     // superseded: ignored
                         edit(a, w, kLinkGestureEnd, 4),                                      // stale release: ignored
                         edit(b, w, kLinkGestureEnd, 5)},
             out);
    CHECK(describe(out) == "B V1 E B V2 V5 E");
    CHECK(rx.openGestures() == 0);
}

TEST_CASE("gestures never nest, even when one sender begins twice") {
    const Uuid a = Uuid::generate();
    LinkEditReceiver rx;
    std::vector<LinkEditAction> out;
    rx.apply(std::vector{edit(a, EncoderParam::RenderWidth, kLinkGestureBegin, 1),
                         edit(a, EncoderParam::RenderWidth, kLinkGestureBegin, 2),
                         edit(a, EncoderParam::RenderWidth, kLinkGestureEnd, 3)},
             out);
    CHECK(describe(out) == "B V1 E B V2 V3 E");
}

TEST_CASE("a sender that disappears mid-gesture has it closed for it") {
    const Uuid a = Uuid::generate(), b = Uuid::generate();
    LinkEditReceiver rx;
    std::vector<LinkEditAction> out;
    rx.apply(std::vector{edit(a, EncoderParam::RenderWidth, kLinkGestureBegin, 1),
                         edit(b, EncoderParam::RenderGain, kLinkGestureBegin, 2)},
             out);
    REQUIRE(rx.openGestures() == 2);

    LinkPeer stillHere;
    stillHere.instance = b;
    out.clear();
    rx.closeAbandoned(std::vector{stillHere}, out);
    CHECK(describe(out) == "E!");
    REQUIRE(out.size() == 1);
    CHECK(out[0].param == EncoderParam::RenderWidth);  // only the one whose sender left
    CHECK_FALSE(rx.isOpen(EncoderParam::RenderWidth));
    CHECK(rx.isOpen(EncoderParam::RenderGain));

    out.clear();
    rx.closeAbandoned(std::span<const LinkPeer>{}, out);
    CHECK(describe(out) == "E!");
    CHECK(rx.openGestures() == 0);
}

TEST_CASE("a parameter this build lacks never reaches the host") {
    LinkEditReceiver rx;
    std::vector<LinkEditAction> out;
    LinkCommand c;
    c.param = 999999;
    c.gesture = kLinkGestureBegin;
    rx.apply(std::vector{c}, out);
    CHECK(out.empty());
    CHECK(rx.openGestures() == 0);
}

/*  The property the host actually depends on, over traffic nobody wrote by hand: three senders
 *  editing three parameters with random begins, values and ends, and senders leaving at random.
 *  Replayed through a host-side state machine, it must never see a value outside a gesture, a
 *  gesture nested inside another, or -- once everyone has gone -- a gesture left open. */
TEST_CASE("whatever arrives, the host only ever sees well-formed gestures") {
    std::mt19937 rng(20260911);
    const Uuid senders[3] = {Uuid::generate(), Uuid::generate(), Uuid::generate()};
    const ParamId params[3] = {EncoderParam::RenderWidth, EncoderParam::RenderGain, EncoderParam::MotionSpeed};

    LinkEditReceiver rx;
    std::vector<LinkEditAction> out;
    std::array<bool, static_cast<std::size_t>(kNumEncoderParams)> hostOpen{};
    int violations = 0, begins = 0, ends = 0;

    const auto replay = [&](std::size_t from) {
        for (std::size_t i = from; i < out.size(); ++i) {
            const LinkEditAction& a = out[i];
            bool& open = hostOpen[static_cast<std::size_t>(a.param)];
            switch (a.kind) {
                case LinkEditAction::Kind::Begin:
                    violations += open ? 1 : 0;
                    open = true;
                    ++begins;
                    break;
                case LinkEditAction::Kind::Value: violations += open ? 0 : 1; break;
                case LinkEditAction::Kind::End:
                    violations += open ? 0 : 1;
                    open = false;
                    ++ends;
                    break;
            }
        }
    };

    std::vector<LinkPeer> live;
    for (int step = 0; step < 20000; ++step) {
        const std::size_t before = out.size();
        if (rng() % 50 == 0) {
            live.clear();
            for (const Uuid& u : senders)
                if (rng() % 3 != 0) {
                    LinkPeer p;
                    p.instance = u;
                    live.push_back(p);
                }
            rx.closeAbandoned(live, out);
        } else {
            const LinkCommand c = edit(senders[rng() % 3], params[rng() % 3], static_cast<std::uint32_t>(rng() % 3),
                                       static_cast<float>(step));
            rx.apply(std::span<const LinkCommand>(&c, 1), out);
        }
        replay(before);
    }
    const std::size_t before = out.size();
    rx.closeAbandoned(std::span<const LinkPeer>{}, out);  // everyone has gone
    replay(before);

    INFO(begins, " gestures replayed");
    CHECK(violations == 0);
    CHECK(begins == ends);  // nothing left open on the host
    CHECK(rx.openGestures() == 0);
    CHECK(begins > 1000);  // the traffic actually exercised it
}

// ------------------------------------------------------------------- duplicated instance ids

TEST_CASE("a duplicated instance id is refused while the original lives, and says so") {
    //  A duplicated track carries its original's state, instance id included.
    Session s;
    const Uuid id = Uuid::generate();
    LinkBus original, copy;
    REQUIRE(original.open(s.id, id, Product::Encoder));

    CHECK_FALSE(copy.open(s.id, id, Product::Encoder));
    CHECK(copy.instanceConflict());
    CHECK_FALSE(copy.isOpen());
    CHECK_FALSE(original.instanceConflict());
    CHECK(original.slotCount() == 1);  // the refused copy left no slot behind

    const Uuid fresh = Uuid::generate();
    REQUIRE(copy.open(s.id, fresh, Product::Encoder));
    CHECK_FALSE(copy.instanceConflict());
    CHECK(original.slotCount() == 2);

    std::vector<LinkPeer> peers;
    original.poll(peers);
    REQUIRE(peers.size() == 2);
    CHECK_FALSE(peers[0].instance == peers[1].instance);
}

TEST_CASE("an instance id is free again once its holder has gone") {
    Session s;
    const Uuid id = Uuid::generate();
    LinkBus first, second;
    REQUIRE(first.open(s.id, id, Product::Encoder));
    first.close();
    CHECK(second.open(s.id, id, Product::Encoder));
    CHECK_FALSE(second.instanceConflict());
}

TEST_CASE("two duplicates joining at the same moment never both keep the id") {
    int bothKept = 0, rounds = 0;
    for (int round = 0; round < 200; ++round) {
        Session s;
        const Uuid id = Uuid::generate();
        LinkBus a, b;
        std::atomic<int> kept{0};
        std::thread ta([&] {
            if (a.open(s.id, id, Product::Encoder)) kept.fetch_add(1);
        });
        std::thread tb([&] {
            if (b.open(s.id, id, Product::Encoder)) kept.fetch_add(1);
        });
        ta.join();
        tb.join();
        if (kept.load() > 1) ++bothKept;
        ++rounds;
    }
    CHECK(rounds == 200);
    CHECK(bothKept == 0);
}

TEST_CASE("instances joining at once never share a slot, even while one of them is still claiming") {
    //  join() claims a slot, then stamps its heartbeat. In between, the slot is occupied with a
    //  heartbeat that reads as dead, and a reaper running inside another joiner could free it and
    //  hand it to that second joiner -- two instances writing one slot. A fresh segment makes it
    //  likeliest: every heartbeat starts at zero. Twelve joiners released together, many times over.
    constexpr int kRounds = 300;
    constexpr int kJoiners = 12;
    int shared = 0, rounds = 0;
    for (int round = 0; round < kRounds; ++round) {
        Session s;
        std::array<LinkBus, kJoiners> buses;
        std::atomic<bool> go{false};
        std::vector<std::thread> threads;
        for (int k = 0; k < kJoiners; ++k)
            threads.emplace_back([&, k] {
                while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                buses[static_cast<std::size_t>(k)].open(s.id, Uuid::generate(), Product::Encoder);
            });
        go.store(true, std::memory_order_release);
        for (auto& t : threads) t.join();

        std::array<int, kLinkMaxInstances> owners{};
        for (const auto& b : buses)
            if (b.isOpen() && b.selfSlot() >= 0) ++owners[static_cast<std::size_t>(b.selfSlot())];
        for (int n : owners)
            if (n > 1) ++shared;
        ++rounds;
    }
    CHECK(rounds == kRounds);
    CHECK(shared == 0);
}

// ------------------------------------------------------------ controlling another instance

namespace {

/// A patch using every part of the format, at its limits.
PluginState fullPatch() {
    PluginState s{encodeParams()};
    s.trajectory.kind = TrajectoryKind::Custom;
    s.trajectory.closed = false;
    s.trajectory.generator = GeneratorType::Spiral;
    for (std::size_t i = 0; i < s.trajectory.genParams.size(); ++i)
        s.trajectory.genParams[i] = 1.0 / 3.0 + 0.1 * static_cast<double>(i);
    for (int i = 0; i < kMaxNodes; ++i) {
        const double a = 0.1 * i;
        Node n;
        n.p = unit({std::cos(a), std::sin(a), 0.3});
        n.cin = unit({std::cos(a - 0.01), std::sin(a - 0.01), 0.29});
        n.cout = unit({std::cos(a + 0.01), std::sin(a + 0.01), 0.31});
        n.smooth = (i % 3) != 0;
        s.trajectory.nodes.push_back(n);
    }
    //  Every (source, target) pair: the most a valid matrix can hold. Depths never zero, both signs.
    int k = 0;
    for (int tab = 0; tab <= static_cast<int>(MatrixTab::Region); ++tab)
        for (int column = 0; column < columnsIn(static_cast<MatrixTab>(tab)); ++column)
            for (int id = 0; id < kNumEncoderParams; ++id) {
                if (!isModulationTarget(encodeMod(), static_cast<ParamId>(id))) continue;
                const double depth = (0.05 + 0.9 * k / 180.0) * ((k % 2) != 0 ? -1.0 : 1.0);
                setCellDepth(encodeMod(), s, static_cast<MatrixTab>(tab), column, static_cast<ParamId>(id), depth);
                ++k;
            }
    s.envTriggers[1].input = TriggerInput::Audio;
    setTriggerSource(s.envTriggers[1], 7);
    setTriggerThreshold(s.envTriggers[1], 0.6);
    setTriggerHysteresis(s.envTriggers[1], 0.25);
    s.envTriggers[2].gate = TriggerGate::Held;
    setTriggerHigh(s.envTriggers[2], 50);
    setTriggerLow(s.envTriggers[2], 40);
    setTriggerChannel(s.envTriggers[2], 10);
    setTriggerVelocity(s.envTriggers[2], 0.75);
    s.sources[7].input = SourceInput::Link;
    s.sources[7].link = Uuid::generate();
    return s;
}

bool samePatch(const PluginState& a, const PluginState& b) {
    const auto& x = a.trajectory;
    const auto& y = b.trajectory;
    if (x.kind != y.kind || x.closed != y.closed || x.generator != y.generator || x.genParams != y.genParams)
        return false;
    if (x.nodes.size() != y.nodes.size()) return false;
    for (std::size_t i = 0; i < x.nodes.size(); ++i) {
        const Node& m = x.nodes[i];
        const Node& n = y.nodes[i];
        const auto same = [](const Vec3& u, const Vec3& v) { return u.x == v.x && u.y == v.y && u.z == v.z; };
        if (!same(m.p, n.p) || !same(m.cin, n.cin) || !same(m.cout, n.cout) || m.smooth != n.smooth) return false;
    }
    if (a.matrix.size() != b.matrix.size()) return false;
    for (std::size_t i = 0; i < a.matrix.size(); ++i) {
        const MatrixCell& m = a.matrix[i];
        const MatrixCell& n = b.matrix[i];
        if (m.tab != n.tab || m.source != n.source || m.target != n.target || m.depth != n.depth) return false;
    }
    if (a.envTriggers != b.envTriggers) return false;
    for (std::size_t i = 0; i < a.sources.size(); ++i)
        if (a.sources[i].input != b.sources[i].input || !(a.sources[i].link == b.sources[i].link)) return false;
    return true;
}

/// Controls whose every field is a fixed multiple of `n`, so a torn read is an exact question.
void controlsAt(LinkControls& c, int n) {
    c.paramCount = static_cast<std::uint32_t>(kNumEncoderParams);
    for (auto& v : c.params) v = static_cast<float>(n);
    c.appliedEdit = static_cast<std::uint32_t>(n);
    for (auto& g : c.patch.genParams) g = n;
    for (auto& node : c.patch.nodes) {
        node.p[0] = n;
        node.cout[2] = 2.0 * n;
    }
    for (auto& cell : c.patch.cells) cell.depth = 3.0 * n;
}

bool consistent(const LinkControls& c) {
    const double n = c.appliedEdit;
    for (const auto v : c.params)
        if (v != static_cast<float>(n)) return false;
    for (const auto g : c.patch.genParams)
        if (g != n) return false;
    for (const auto& node : c.patch.nodes)
        if (node.p[0] != n || node.cout[2] != 2.0 * n) return false;
    for (const auto& cell : c.patch.cells)
        if (cell.depth != 3.0 * n) return false;
    return true;
}

void editAt(LinkPatchEdit& e, int n) {
    e.setName(std::to_string(n));
    for (auto& g : e.patch.genParams) g = n;
    for (auto& node : e.patch.nodes) node.cin[1] = -1.0 * n;
}

bool consistent(const LinkPatchEdit& e) {
    const double n = e.patch.genParams[0];
    if (e.nameString() != std::to_string(static_cast<int>(n))) return false;
    for (const auto g : e.patch.genParams)
        if (g != n) return false;
    for (const auto& node : e.patch.nodes)
        if (node.cin[1] != -n) return false;
    return true;
}

}  // namespace

TEST_CASE("the patch format holds the largest matrix the core can make") {
    int targets = 0;
    for (int id = 0; id < kNumEncoderParams; ++id)
        targets += isModulationTarget(encodeMod(), static_cast<ParamId>(id)) ? 1 : 0;
    CHECK(kLinkMaxCells == kNumSources * targets);
    CHECK(kLinkMaxParams >= kNumEncoderParams);
    CHECK(kLinkTriggers == kNumEnvelopes);
}

TEST_CASE("a patch travels whole: every node and every cell, bit for bit") {
    const PluginState sent = fullPatch();
    REQUIRE(sent.matrix.size() == static_cast<std::size_t>(kLinkMaxCells));
    REQUIRE(sent.trajectory.nodes.size() == static_cast<std::size_t>(kMaxNodes));

    auto packed = std::make_unique<LinkPatch>();
    packPatch(sent, *packed);
    PluginState received{encodeParams()};
    received.identity.instance = Uuid::generate();
    received.params[static_cast<std::size_t>(EncoderParam::MotionSpeed)] = 42.0f;
    const Uuid itself = received.identity.instance;
    REQUIRE(unpackPatch(encodeMod(), *packed, received));
    CHECK(samePatch(sent, received));

    //  The target's own identity and host parameters are not part of a patch.
    CHECK(received.identity.instance == itself);
    CHECK(received.params[static_cast<std::size_t>(EncoderParam::MotionSpeed)] == 42.0f);
}

TEST_CASE("a patch from another process is validated before any of it is used") {
    auto p = std::make_unique<LinkPatch>();
    const auto refused = [&](void (*spoil)(LinkPatch&)) {
        packPatch(fullPatch(), *p);
        spoil(*p);
        PluginState target{encodeParams()};
        return !unpackPatch(encodeMod(), *p, target) && samePatch(target, PluginState{encodeParams()});
    };
    CHECK(refused([](LinkPatch& x) { x.nodeCount = kMaxNodes + 1; }));
    CHECK(refused([](LinkPatch& x) { x.cellCount = kLinkMaxCells + 1; }));
    CHECK(refused([](LinkPatch& x) { x.nodes[3].p[1] = std::numeric_limits<double>::quiet_NaN(); }));
    CHECK(refused([](LinkPatch& x) { x.genParams[0] = std::numeric_limits<double>::infinity(); }));
    CHECK(refused([](LinkPatch& x) { x.triggers[1].threshold = std::numeric_limits<double>::quiet_NaN(); }));

    //  What can be repaired is.
    PluginState small{encodeParams()};
    setCellDepth(encodeMod(), small, MatrixTab::Features, 0, EncoderParam::RenderWidth, 0.5);
    packPatch(small, *p);
    p->generator = 200;
    p->kind = 9;
    p->cells[1] = {1, 9, static_cast<std::uint16_t>(EncoderParam::RenderWidth), 0, 0.5};      // no column 9
    p->cells[2] = {0, 0, static_cast<std::uint16_t>(EncoderParam::MotionDirection), 0, 0.5};  // not a target
    p->cells[3] = {0, 0, static_cast<std::uint16_t>(EncoderParam::RenderWidth), 0, 7.0};  // a duplicate, out of range
    p->cellCount = 4;
    p->triggers[0].noteLow = 90;
    p->triggers[0].noteHigh = 40;
    p->triggers[0].channel = 200;
    p->triggers[0].threshold = 0.2;
    p->triggers[0].hysteresis = 0.9;
    p->triggers[0].source = 99;
    p->sources[0].input = 77;

    PluginState s{encodeParams()};
    REQUIRE(unpackPatch(encodeMod(), *p, s));
    CHECK(s.trajectory.generator == GeneratorType::Spiral);
    CHECK(s.trajectory.kind == TrajectoryKind::Parametric);
    REQUIRE(s.matrix.size() == 1);
    CHECK(s.matrix[0].depth == 1.0);
    const EnvTrigger& t = s.envTriggers[0];
    CHECK(t.noteLow <= t.noteHigh);
    CHECK(t.channel == 16);
    CHECK(t.hysteresis <= t.threshold);
    CHECK(t.source == kTriggerSources - 1);
    CHECK(s.sources[0].input == SourceInput::Link);
}

TEST_CASE("a peer's controls and source values are there once published, and a new occupant starts without them") {
    Session s;
    const Uuid idA = Uuid::generate(), idViewer = Uuid::generate();
    LinkBus viewer;
    std::vector<LinkPeer> peers;
    auto controls = std::make_unique<LinkControls>();
    auto edit = std::make_unique<LinkPatchEdit>();
    std::uint32_t generation = 0, last = 0;

    int slotA = -1;
    {
        LinkBus a;
        REQUIRE(a.open(s.id, idA, Product::Encoder));
        REQUIRE(viewer.open(s.id, idViewer, Product::Encoder));
        slotA = a.selfSlot();

        viewer.poll(peers);
        auto peer = std::find_if(peers.begin(), peers.end(), [&](const LinkPeer& p) { return p.instance == idA; });
        REQUIRE(peer != peers.end());
        CHECK(peer->controlsGeneration == 0);
        CHECK_FALSE(viewer.readControls(*peer, *controls, generation));

        controlsAt(*controls, 5);
        a.publishControls(*controls);
        LinkDynamic d;
        d.sources[12] = 0.5f;
        a.publishDynamic(d);

        viewer.poll(peers);
        peer = std::find_if(peers.begin(), peers.end(), [&](const LinkPeer& p) { return p.instance == idA; });
        REQUIRE(peer != peers.end());
        CHECK(peer->controlsGeneration == 1);
        CHECK(peer->dyn.sources[12] == 0.5f);
        controlsAt(*controls, 0);
        REQUIRE(viewer.readControls(*peer, *controls, generation));
        CHECK(generation == 1);
        CHECK(controls->appliedEdit == 5);
        CHECK(consistent(*controls));

        LinkScene scene;
        scene.update(viewer);
        const auto* entry = scene.find(idA);
        REQUIRE(entry != nullptr);
        CHECK(entry->slot == slotA);
        CHECK(entry->controlsGeneration == 1);
        CHECK(viewer.readControls(entry->peer(), *controls, generation));

        CHECK(viewer.sendPatchEdit(idA, *edit) != 0);
    }

    //  The next instance to claim that slot sees neither the old controls nor the old edit.
    const Uuid idC = Uuid::generate();
    LinkBus c;
    REQUIRE(c.open(s.id, idC, Product::Encoder));
    REQUIRE(c.selfSlot() == slotA);
    CHECK_FALSE(c.takePatchEdit(*edit, last));
    //  Numbered from the start again: a stale count would have the new occupant copy the cell every tick.
    CHECK(viewer.sendPatchEdit(idC, *edit) == 1);
    viewer.poll(peers);
    const auto peer = std::find_if(peers.begin(), peers.end(), [&](const LinkPeer& p) { return p.instance == idC; });
    REQUIRE(peer != peers.end());
    CHECK(peer->controlsGeneration == 0);
    CHECK_FALSE(viewer.readControls(*peer, *controls, generation));
}

TEST_CASE("controls and their generation always arrive together") {
    Session s;
    const Uuid writerId = Uuid::generate();
    LinkBus writer, reader;
    REQUIRE(writer.open(s.id, writerId, Product::Encoder));
    REQUIRE(reader.open(s.id, Uuid::generate(), Product::Encoder));

    std::atomic<bool> stop{false};
    std::thread w([&] {
        auto c = std::make_unique<LinkControls>();
        for (int n = 1; n <= 2'000'000 && !stop.load(std::memory_order_relaxed); ++n) {
            controlsAt(*c, n);
            writer.publishControls(*c);
        }
    });

    int reads = 0, torn = 0, mismatched = 0, backwards = 0;
    std::uint32_t lastGen = 0;
    std::vector<LinkPeer> peers;
    auto c = std::make_unique<LinkControls>();
    const auto until = std::chrono::steady_clock::now() + kReadDeadline;
    while (reads < kEnoughReads && std::chrono::steady_clock::now() < until) {
        reader.poll(peers);
        for (const auto& p : peers) {
            if (!(p.instance == writerId)) continue;
            std::uint32_t gen = 0;
            if (!reader.readControls(p, *c, gen)) continue;
            ++reads;
            if (!consistent(*c)) ++torn;
            if (c->appliedEdit != gen) ++mismatched;
            if (gen < lastGen) ++backwards;
            lastGen = gen;
        }
    }
    stop.store(true);
    w.join();

    INFO("checked ", reads, " controls, last generation ", lastGen);
    CHECK(reads >= kEnoughReads);
    CHECK(torn == 0);
    CHECK(mismatched == 0);
    CHECK(backwards == 0);
}

TEST_CASE("a patch edit reaches its target once, stamped with its sender, and the newest wins") {
    Session s;
    const Uuid targetId = Uuid::generate(), oneId = Uuid::generate(), twoId = Uuid::generate();
    LinkBus target, one, two;
    REQUIRE(target.open(s.id, targetId, Product::Encoder));
    REQUIRE(one.open(s.id, oneId, Product::Encoder));
    REQUIRE(two.open(s.id, twoId, Product::Encoder));

    auto edit = std::make_unique<LinkPatchEdit>();
    auto got = std::make_unique<LinkPatchEdit>();
    std::uint32_t last = 0;
    CHECK_FALSE(target.takePatchEdit(*got, last));

    edit->setName("matrix depth");
    edit->setKey("cell");
    edit->patch.genParams[0] = 1.0;
    const std::uint32_t first = one.sendPatchEdit(targetId, *edit);
    edit->patch.genParams[0] = 2.0;
    edit->flags = kLinkEditEnd;
    const std::uint32_t second = two.sendPatchEdit(targetId, *edit);
    CHECK(first != 0);
    CHECK(second > first);

    REQUIRE(target.takePatchEdit(*got, last));
    CHECK(got->sequence == second);
    CHECK(last == second);
    CHECK(got->from == twoId);
    CHECK(got->patch.genParams[0] == 2.0);  // the first was overwritten before anyone read it: latest wins
    CHECK(got->flags == kLinkEditEnd);
    CHECK(got->nameString() == "matrix depth");
    CHECK(got->keyString() == "cell");
    CHECK_FALSE(target.takePatchEdit(*got, last));

    //  The sender is stamped by the bus, whatever the edit says.
    edit->from = Uuid::generate();
    REQUIRE(one.sendPatchEdit(targetId, *edit) != 0);
    REQUIRE(target.takePatchEdit(*got, last));
    CHECK(got->from == oneId);

    CHECK(one.sendPatchEdit(Uuid::generate(), *edit) == 0);  // nobody by that id
}

TEST_CASE("many windows editing one instance at once never tear an edit") {
    Session s;
    const Uuid targetId = Uuid::generate();
    LinkBus target;
    REQUIRE(target.open(s.id, targetId, Product::Encoder));

    std::atomic<bool> stop{false};
    std::atomic<int> refused{0}, sent{0}, taken{0};
    std::vector<std::thread> senders;
    for (int k = 0; k < 3; ++k) {
        senders.emplace_back([&, k] {
            LinkBus bus;
            if (!bus.open(s.id, Uuid::generate(), Product::Encoder)) return;
            auto e = std::make_unique<LinkPatchEdit>();
            for (int n = 1; !stop.load(std::memory_order_relaxed); ++n) {
                editAt(*e, k * 1'000'000 + n);
                if (bus.sendPatchEdit(targetId, *e) != 0)
                    sent.fetch_add(1);
                else
                    refused.fetch_add(1);
                //  Bursts back to back, where writers collide on the lock, then a pause that lets the reader in.
                //  A window sends once a frame; a burst is thousands of times that.
                if (n % 32 == 0) pauseForReader(taken, std::chrono::milliseconds(2));
            }
        });
    }

    int reads = 0, torn = 0, backwards = 0;
    std::uint32_t last = 0, previous = 0;
    auto got = std::make_unique<LinkPatchEdit>();
    const auto until = std::chrono::steady_clock::now() + kReadDeadline;
    while (reads < kEnoughReads && std::chrono::steady_clock::now() < until) {
        if (!target.takePatchEdit(*got, last)) continue;
        ++reads;
        taken.store(reads, std::memory_order_relaxed);
        if (!consistent(*got)) ++torn;
        if (got->sequence <= previous) ++backwards;
        previous = got->sequence;
    }
    stop.store(true);
    for (auto& t : senders) t.join();

    INFO("read ", reads, " edits of ", sent.load(), " sent, ", refused.load(), " refused");
    CHECK(reads >= kEnoughReads);
    CHECK(torn == 0);
    CHECK(backwards == 0);
}

TEST_CASE("undo, redo, learn and clearing a turn travel as commands, and never reach the host as a parameter") {
    for (const auto command : {kLinkCommandUndo, kLinkCommandRedo, kLinkCommandLearn, kLinkCommandZeroTurn}) {
        LinkCommand c;
        c.param = command;
        c.gesture = kLinkGestureEnd;
        CHECK(c.paramId() == kNoParamId);
        LinkEditReceiver receiver;
        std::vector<LinkEditAction> out;
        receiver.apply(std::span<const LinkCommand>(&c, 1), out);
        CHECK(out.empty());
    }
}

/*  A shared-memory object outlives every process that opened it, and macOS lets one be sized only once,
    so a build meeting an older build's smaller segment could never grow it -- and the 200 ms it spent
    trying, on the message thread, froze every plugin window in the host every two seconds while it
    retried. The name carries the layout version so the two never meet; this pins both halves. */
TEST_CASE("a session's segment is named for the layout it holds, and a stale one is refused at once") {
    const Uuid session = Uuid::generate();
    const std::string name = LinkBus::segmentNameFor(session);
    CHECK(name.find("." + std::to_string(kLinkVersion) + ".") != std::string::npos);
    CHECK(name.size() <= 31);  // macOS caps a POSIX shared-memory name here
    CHECK(LinkBus::segmentNameFor(Uuid::generate()) != name);

    //  a leftover far too small for this build, held open: on Windows a segment lasts only while it is open
    detail::unlinkShared(name);
    std::intptr_t handle = -1;
    void* small = nullptr;
    std::string error;
    REQUIRE(detail::mapShared(name, 4096, handle, small, error));

    LinkBus bus;
    const auto begin = std::chrono::steady_clock::now();
    const bool opened = bus.open(session, Uuid::generate(), Product::Encoder);
    const auto took =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
    INFO("open ", opened ? "succeeded" : "failed", " in ", took, " ms: ", bus.error());
    CHECK(took < 100);  // never a wait a user can see
    bus.close();
    detail::unmapShared(small, 4096, handle);
    LinkBus::unlinkSession(session);
}

TEST_CASE("a path is sent with a point every degree of its length, from 128 to 4096") {
    CHECK(linkPathPointsFor(0.0) == kLinkMinPathPoints);
    CHECK(linkPathPointsFor(90.0 * kDeg2Rad) == kLinkMinPathPoints);  // a short arc still gets the floor
    CHECK(linkPathPointsFor(360.0 * kDeg2Rad) == 360);
    CHECK(linkPathPointsFor(1e6) == kLinkPathPoints);  // and nothing past the array

    //  The case a fixed 128 drew as a polygon: a 7:7 Lissajous at full size. Every point sent lies on the path,
    //  and the drawn line between two of them strays from it by a fraction of a degree, not thirteen.
    TrajectoryState s;
    s.kind = TrajectoryKind::Parametric;
    s.generator = GeneratorType::Lissajous;
    generatorDefaults(s.generator, s.genParams);
    s.genParams[0] = 180.0, s.genParams[1] = 90.0, s.genParams[2] = 7.0, s.genParams[3] = 7.0;
    Trajectory t;
    t.build(s);
    LinkStatic st;
    st.setPath(t);
    const int n = static_cast<int>(st.pointCount);
    REQUIRE(n > 1000);
    double worst = 0.0;
    for (int i = 0; i < n; ++i) {
        const Vec3 a = st.point(i), b = st.point((i + 1) % n);
        const Vec3 drawn = unit(a + (b - a) * 0.5);
        const Vec3 onPath = t.eval((i + 0.5) / (t.closed() ? n : n - 1));
        if (t.closed() || i + 1 < n) worst = std::max(worst, arc(drawn, onPath));
    }
    INFO("worst gap ", worst * kRad2Deg, " deg over ", n, " points");
    CHECK(worst * kRad2Deg < 0.5);
}

TEST_CASE("the bus carries every kind, but an edit never crosses one") {
    /*  One bus carries all three plugins. The scene is kind-agnostic and must stay so: a window draws
        every instance's path whatever made it. What is not kind-agnostic is anything addressed by
        parameter position -- a LinkCommand's `param` is an index into a manifest, and a manifest is a
        plugin's own -- so an index meaning `render.width` in the encoder means something else in Echo
        and would be applied in perfect silence.

        Catches: the product check dropped from sendCommand (an Echo accepts an encoder's parameter
        edit), dropped from sendPatchEdit (an Echo accepts a patch shaped like the encoder's state),
        and the product never written at join (every instance reads as Unknown and matches nothing,
        which the same-kind half of this test catches). */
    Session s;
    LinkBus encoder, echo, otherEncoder;
    const Uuid idEncoder = Uuid::generate(), idEcho = Uuid::generate(), idOther = Uuid::generate();
    REQUIRE(encoder.open(s.id, idEncoder, Product::Encoder));
    REQUIRE(echo.open(s.id, idEcho, Product::Echo));
    REQUIRE(otherEncoder.open(s.id, idOther, Product::Encoder));

    CHECK(encoder.product() == Product::Encoder);
    CHECK(echo.product() == Product::Echo);

    //  the scene sees both kinds, and knows which is which
    std::vector<LinkPeer> peers;
    encoder.poll(peers);
    int sawEcho = 0, sawEncoder = 0;
    for (const LinkPeer& p : peers) {
        if (p.instance == idEcho) {
            ++sawEcho;
            CHECK(p.product == Product::Echo);
        }
        if (p.instance == idOther) {
            ++sawEncoder;
            CHECK(p.product == Product::Encoder);
        }
    }
    CHECK(sawEcho == 1);
    CHECK(sawEncoder == 1);

    LinkCommand c;
    c.from = idEncoder;
    c.param = 0;
    c.value = 0.5f;

    //  to another encoder: it lands
    CHECK(encoder.sendCommand(idOther, c));
    //  to an Echo: refused, and counted rather than silently dropped
    CHECK_FALSE(encoder.sendCommand(idEcho, c));
    //  and the other way round too, so it is not a rule about who is senior
    LinkCommand back;
    back.from = idEcho;
    CHECK_FALSE(echo.sendCommand(idEncoder, back));

    //  a whole patch is shaped like its plugin's own state, so it does not cross either
    LinkPatchEdit edit;
    edit.setName("a name");
    CHECK(encoder.sendPatchEdit(idOther, edit) != 0);
    CHECK(encoder.sendPatchEdit(idEcho, edit) == 0);

    //  what did land is still there to be taken
    std::vector<LinkCommand> got;
    otherEncoder.drainCommands(got);
    CHECK(got.size() == 1);
    std::vector<LinkCommand> none;
    echo.drainCommands(none);
    CHECK(none.empty());
}

TEST_CASE("a patch carries an effect's state across the bus") {
    /*  The bus's patch is shaped like the encoder's: a trajectory, cells, triggers, sources. An
        effect has no trajectory and its own state is its regions -- so a remote window could edit
        an effect's parameters and not the one thing it actually has. */
    auto packed = std::make_unique<LinkPatch>();

    SUBCASE("the regions and the render quality round-trip") {
        //  Catches: dropping either loop, which leaves the shapes at their defaults.
        PluginState sent{echoParams()};
        sent.regions[0].shape = {RegionKind::Dots, 4, 12};
        sent.regions[1].shape = {RegionKind::Sectors, 7, 6};
        sent.renderQuality = RenderQuality::Same;
        packPatch(sent, *packed);

        PluginState got{echoParams()};
        REQUIRE(unpackPatch(echoMod(), *packed, got));
        CHECK(got.regions[0].shape.kind == RegionKind::Dots);
        CHECK(got.regions[0].shape.dots == 12);
        CHECK(got.regions[1].shape.kind == RegionKind::Sectors);
        CHECK(got.regions[1].shape.sectors == 7);
        CHECK(got.renderQuality == RenderQuality::Same);
    }

    SUBCASE("a shape another build sent is repaired, not refused") {
        /*  Through `sanitised()`, the one place that decides what an evaluable shape is -- so a
            region that crossed the bus cannot differ from one that came out of a state file. A kind
            this build has never heard of, seven dots, and no sectors at all. */
        PluginState sent{echoParams()};
        packPatch(sent, *packed);
        packed->regions[0].kind = 200;
        packed->regions[1].kind = static_cast<std::uint8_t>(RegionKind::Dots);
        packed->regions[1].dots = 7;
        packed->regions[2].kind = static_cast<std::uint8_t>(RegionKind::Sectors);
        packed->regions[2].sectors = 0;

        PluginState got{echoParams()};
        REQUIRE(unpackPatch(echoMod(), *packed, got));
        CHECK(got.regions[0].shape.kind <= RegionKind::Custom);
        CHECK(got.regions[1].shape.dots == 6);  // seven is not a solid
        CHECK(got.regions[2].shape.sectors >= 1);
    }

    SUBCASE("a quality byte this build does not know reads as the default") {
        PluginState sent{echoParams()};
        packPatch(sent, *packed);
        packed->renderQuality = 99;
        PluginState got{echoParams()};
        got.renderQuality = RenderQuality::Same;
        REQUIRE(unpackPatch(echoMod(), *packed, got));
        CHECK(got.renderQuality == RenderQuality::Realistic);
    }
}

TEST_CASE("a peer is called by its label, or by its id until it has one") {
    //  Every window's fallback. Catches: the label read before its static section has arrived, or
    //  the fallback lost.
    LinkScene::Entry e;
    e.instance = *Uuid::parse("ab12cd34-0000-4000-8000-000000000000");
    e.st.setLabel("kick");
    CHECK(e.label() == "instance ab12");  // generation 0: the static section is not in yet
    e.generation = 1;
    CHECK(e.label() == "kick");
    e.st.setLabel("");
    CHECK(e.label() == "instance ab12");
}

TEST_CASE("an instance's regions cross the bus as its owner resolved them") {
    /*  Each kind round trips through a real bus: the receiver's region answers what the sender's did.
        Catches: a field left out of `set` or `region` (the spot's size, the sectors' fill, a dot
        size, a custom weight), and an effect that publishes counted as a source to draw. */
    Session s;
    const Uuid idA = Uuid::generate(), idB = Uuid::generate();
    LinkBus a, b;
    REQUIRE(a.open(s.id, idA, Product::Reverb));
    REQUIRE(b.open(s.id, idB, Product::Encoder));

    Region spot;
    spot.kind = RegionKind::Spot;
    spot.size = 0.7;
    spot.softness = 0.2;
    spot.yaw = 1.1;
    spot.pitch = 0.3;
    Region sectors;
    sectors.kind = RegionKind::Sectors;
    sectors.sectors = 5;
    sectors.fill = 0.3;
    sectors.roll = 0.4;
    Region dots;
    dots.kind = RegionKind::Dots;
    dots.dots = 8;
    dots.dotSize = 0.25;
    Region custom;
    custom.kind = RegionKind::Custom;
    custom.weights[0] = 0.5;
    custom.weights[3] = 0.4;  // ACN 3: x, towards the front

    LinkDynamic d;
    d.regionCount = 4;
    d.regions[0].set(spot);
    d.regions[1].set(sectors);
    d.regions[2].set(dots);
    d.regions[3].set(custom);
    a.publishDynamic(d);

    LinkScene scene;
    scene.update(b);
    const auto* e = scene.find(idA);
    REQUIRE(e != nullptr);
    REQUIRE(e->dyn.regionCount == 4);
    CHECK_FALSE(e->hasPosition);  // a Reverb publishes, and still has no source to draw

    const Region sent[] = {spot, sectors, dots, custom};
    //  200 directions spread over the sphere, so every edge -- a dot's included -- is crossed somewhere
    std::vector<Vec3> probes;
    const double golden = kPi * (3.0 - std::sqrt(5.0));
    for (int k = 0; k < 200; ++k) {
        const double z = 1.0 - 2.0 * (k + 0.5) / 200.0, r = std::sqrt(1.0 - z * z);
        probes.push_back({r * std::cos(k * golden), r * std::sin(k * golden), z});
    }
    for (int i = 0; i < 4; ++i) {
        const Region got = e->dyn.regions[i].region();
        CAPTURE(i);
        CHECK(got.kind == sent[i].kind);
        double worst = 0.0;
        for (const Vec3& p : probes) worst = std::max(worst, std::abs(valueAt(got, p) - valueAt(sent[i], p)));
        CHECK(worst < 1e-4);  // floats on the bus
    }
}

TEST_CASE("an instance's energy is asked for, published and read by generation, and a new occupant starts unasked") {
    Session s;
    const Uuid idA = Uuid::generate(), idViewer = Uuid::generate();
    LinkBus viewer;
    std::vector<LinkPeer> peers;
    auto energy = std::make_unique<LinkEnergy>();
    auto got = std::make_unique<LinkEnergy>();
    std::uint32_t generation = 0;
    REQUIRE(viewer.open(s.id, idViewer, Product::Echo));
    const auto peerOf = [&](const Uuid& id) {
        viewer.poll(peers);
        auto p = std::find_if(peers.begin(), peers.end(), [&](const LinkPeer& q) { return q.instance == id; });
        REQUIRE(p != peers.end());
        return *p;
    };

    int slotA = -1;
    {
        LinkBus a;
        REQUIRE(a.open(s.id, idA, Product::Echo));
        slotA = a.selfSlot();
        const LinkPeer peer = peerOf(idA);
        //  Catches a slot claimed with a request or a summary left in it.
        CHECK(a.energyAskedUntil() == 0);
        CHECK_FALSE(viewer.readEnergy(peer, *got, generation));

        const auto now = linkNowMicros();
        REQUIRE(viewer.askForEnergy(peer, now + 1'000'000));
        //  Catches a later, shorter ask cutting a longer one short: two viewers each keep it alive.
        viewer.askForEnergy(peer, now + 10);
        CHECK(a.energyAskedUntil() == now + 1'000'000);

        energy->order = 3;
        energy->withArrived = 1;
        energy->count = kLinkEnergyValues;
        energy->added[0] = 2.0f;
        energy->arrived[135] = 0.25f;
        a.publishEnergy(*energy);
        REQUIRE(viewer.readEnergy(peer, *got, generation));
        const auto first = generation;
        CHECK(got->order == 3);
        CHECK(got->added[0] == 2.0f);
        CHECK(got->arrived[135] == 0.25f);

        energy->added[0] = 3.0f;
        a.publishEnergy(*energy);
        REQUIRE(viewer.readEnergy(peer, *got, generation));
        //  Catches a generation that does not move, which a viewer would read as nothing new.
        CHECK(generation != first);
        CHECK(got->added[0] == 3.0f);
    }
    //  the slot is free again; the next instance in it has been asked for nothing
    LinkBus b;
    const Uuid idB = Uuid::generate();
    REQUIRE(b.open(s.id, idB, Product::Echo));
    if (b.selfSlot() == slotA) {
        CHECK(b.energyAskedUntil() == 0);
        CHECK_FALSE(viewer.readEnergy(peerOf(idB), *got, generation));
    }
}

TEST_CASE("an instance's undo and redo names cross the bus with its controls") {
    //  Catches: the names dropped from the controls, swapped, or not cleared when the history empties.
    Session s;
    const Uuid writerId = Uuid::generate();
    LinkBus writer, reader;
    REQUIRE(writer.open(s.id, writerId, Product::Encoder));
    REQUIRE(reader.open(s.id, Uuid::generate(), Product::Encoder));

    auto sent = std::make_unique<LinkControls>(), got = std::make_unique<LinkControls>();
    const auto readBack = [&] {
        std::vector<LinkPeer> peers;
        reader.poll(peers);
        std::uint32_t gen = 0;
        for (const auto& p : peers)
            if (p.instance == writerId) return reader.readControls(p, *got, gen);
        return false;
    };

    sent->canUndo = sent->canRedo = 1;
    sent->setUndoNames("set width", "delete node");
    writer.publishControls(*sent);
    REQUIRE(readBack());
    CHECK(got->undoNameString() == "set width");
    CHECK(got->redoNameString() == "delete node");

    sent->setUndoNames("", "");
    writer.publishControls(*sent);
    REQUIRE(readBack());
    CHECK(got->undoNameString().empty());
    CHECK(got->redoNameString().empty());
}
