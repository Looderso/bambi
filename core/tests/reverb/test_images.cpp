// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cmath>
#include <memory>

#include "bambi/reverb/images.hpp"
#include "doctest.h"

using namespace bambi;

namespace {
Room hall() { return deriveRoom({20.0, RoomShape::Hall, 1.9, 0.5, 0.6}); }
}  // namespace

TEST_CASE("a box has 6, 24 and 62 images by order 1, 2 and 3") {
    std::array<ImageIndex, kMaxImages> index{};
    CHECK(imageIndices(1, index) == 6);
    CHECK(imageIndices(2, index) == 24);
    CHECK(imageIndices(3, index) == 62);
    int byOrder[4] = {0, 0, 0, 0};
    for (int i = 0; i < 62; ++i) {
        const ImageIndex& n = index[static_cast<std::size_t>(i)];
        CHECK(std::abs(n.nx) + std::abs(n.ny) + std::abs(n.nz) == n.order);
        ++byOrder[n.order];
    }
    CHECK(byOrder[1] == 6);
    CHECK(byOrder[2] == 18);
    CHECK(byOrder[3] == 38);
}

TEST_CASE("the first reflection off the front wall is where a mirror puts it") {
    //  A source 6 m straight ahead in a hall 36 m deep: the front wall is 18 m away, so its image is at
    //  18 + 12 = 30 m, straight ahead -- 24 m farther than the source, 70 ms late, a fifth as loud before
    //  the wall's loss. Catches the mirror formula, the delay taken from the image rather than the extra
    //  path, and the wall's loss left out or squared.
    const Room room = hall();
    auto taps = std::make_unique<std::array<Reflection, kMaxImages>>();
    REQUIRE(reflectionsOf(room, {1, 0, 0}, 6.0, *taps) == 62);
    std::array<ImageIndex, kMaxImages> index{};
    imageIndices(3, index);
    bool found = false;
    for (int i = 0; i < 62; ++i) {
        const ImageIndex& n = index[static_cast<std::size_t>(i)];
        if (n.nx != 1 || n.ny != 0 || n.nz != 0) continue;
        const Reflection& t = (*taps)[static_cast<std::size_t>(i)];
        found = true;
        CHECK(t.distance == doctest::Approx(30.0));
        CHECK(arc(t.direction, {1, 0, 0}) < 1e-9);
        CHECK(t.delay == doctest::Approx(24.0 / 343.0));
        CHECK(t.gain == doctest::Approx(6.0 / 30.0 * std::sqrt(1.0 - room.absorbMid)));
        CHECK(t.order == 1);
    }
    CHECK(found);
    //  the floor's, from a source at ear height in the middle of the room: straight below
    for (int i = 0; i < 62; ++i)
        if (index[static_cast<std::size_t>(i)].nx == 0 && index[static_cast<std::size_t>(i)].ny == 0 &&
            index[static_cast<std::size_t>(i)].nz == -1) {
            const Reflection& t = (*taps)[static_cast<std::size_t>(i)];
            CHECK(t.direction.z < 0.0);
            CHECK(t.distance == doctest::Approx(std::hypot(6.0, 12.0)));  // 6 m ahead, 12 m down through the floor
        }
}

TEST_CASE("what stops being a reflection is not lost: mirror and scattered are the image's whole energy") {
    const Room room = hall();
    auto taps = std::make_unique<std::array<Reflection, kMaxImages>>();
    reflectionsOf(room, unit(Vec3{0.6, 0.7, 0.2}), 6.0, *taps);
    int faded = 0, whole = 0, between = 0;
    for (const Reflection& t : *taps) {
        CHECK(t.mirror * t.mirror + t.scattered * t.scattered == doctest::Approx(t.gain * t.gain));
        //  scattering alone leaves (1 - s)^order a mirror image, and past the mixing time nothing is
        const double unfaded = t.gain * std::sqrt(std::pow(1.0 - room.scatter, t.order));
        if (t.delay < 0.7 * room.mixingTime) {
            CHECK(t.mirror == doctest::Approx(unfaded));
            ++whole;
        }
        if (t.delay > 1.3 * room.mixingTime) {
            CHECK(t.mirror == 0.0);
            ++faded;
        }
        if (t.delay > 0.7 * room.mixingTime && t.delay < 1.3 * room.mixingTime) {
            //  a raised cosine in amplitude across the mixing time: half way through it, half the pressure
            const double w = 0.5 + 0.5 * std::cos(kPi * (t.delay - 0.7 * room.mixingTime) / (0.6 * room.mixingTime));
            CHECK(t.mirror == doctest::Approx(unfaded * w));
            ++between;
        }
    }
    CHECK(whole > 0);
    CHECK(between > 0);
    CHECK(faded > 0);  // a hall's later images run past its 80 ms: those are the ones heard as echoes
}

TEST_CASE("every bounce and every metre dulls a reflection, between 800 Hz and 20 kHz") {
    const Room dark = deriveRoom({20.0, RoomShape::Hall, 1.9, 0.0, 0.6});
    auto taps = std::make_unique<std::array<Reflection, kMaxImages>>();
    reflectionsOf(dark, {1, 0, 0}, 6.0, *taps);
    double first = 0.0, third = 0.0;
    for (const Reflection& t : *taps) {
        CHECK(t.cutoffHz >= 800.0);
        CHECK(t.cutoffHz <= 20000.0);
        if (t.order == 1) first = std::max(first, t.cutoffHz);
        if (t.order == 3) third = std::max(third, t.cutoffHz);
    }
    CHECK(third < first);
    //  with walls that treat the highs like the mids, only the air is left: 16 kHz x sqrt(7.5 / path)
    const Room even = deriveRoom({20.0, RoomShape::Hall, 1.9, 1.0, 0.6});
    reflectionsOf(even, {1, 0, 0}, 6.0, *taps);
    for (const Reflection& t : *taps)
        CHECK(t.cutoffHz ==
              doctest::Approx(std::clamp(16000.0 * std::sqrt(7.5 / std::max(t.distance, 7.5)), 800.0, 20000.0)));
}

TEST_CASE("a source is kept inside the room, no nearer a wall than a tenth of the way") {
    const Room small = deriveRoom({4.0, RoomShape::Room, 0.35, 0.5, 0.8});  // 5 x 4 x 2.6 m
    CHECK(sourceRadius(small, {1, 0, 0}, 1.5) == 1.5);
    CHECK(sourceRadius(small, {1, 0, 0}, 9.0) == doctest::Approx(0.9 * 2.5));
    CHECK(sourceRadius(small, {0, 0, 1}, 9.0) == doctest::Approx(0.9 * 1.3));
    CHECK(sourceRadius(small, unit(Vec3{1, 1, 0}), 9.0) == doctest::Approx(0.9 * 2.0 * std::sqrt(2.0)));
}

TEST_CASE("the beam is 1 along itself and what max-rE says elsewhere; the virtual sources are spread") {
    CHECK(maxReBeam(3, 1.0) == doctest::Approx(1.0));
    CHECK(maxReBeam(1, 1.0) == doctest::Approx(1.0));
    CHECK(std::abs(maxReBeam(3, -1.0)) < 0.15);  // little behind
    CHECK(maxReBeam(3, std::cos(0.5)) < maxReBeam(3, std::cos(0.25)));
    //  order 1, by hand: (1 + 3 w1 c) / (1 + 3 w1), w1 = cos(137.9 / 2.51 degrees)
    const double w1 = std::cos(137.9 * kDeg2Rad / 2.51);
    CHECK(maxReBeam(1, 0.3) == doctest::Approx((1.0 + 3.0 * w1 * 0.3) / (1.0 + 3.0 * w1)));

    //  and order 2, where the recurrence for the weights first has something to get wrong
    const double at2 = std::cos(137.9 * kDeg2Rad / 3.51), v1 = at2, v2 = (3.0 * at2 * at2 - 1.0) / 2.0, c = -0.4;
    CHECK(maxReBeam(2, c) ==
          doctest::Approx((1.0 + 3.0 * v1 * c + 5.0 * v2 * (3.0 * c * c - 1.0) / 2.0) / (1.0 + 3.0 * v1 + 5.0 * v2)));

    const auto dirs = virtualSourceDirections();
    double nearest = 10.0;
    Vec3 sum{0, 0, 0};
    for (std::size_t i = 0; i < dirs.size(); ++i) {
        CHECK(length(dirs[i]) == doctest::Approx(1.0));
        sum = sum + dirs[i];
        for (std::size_t j = i + 1; j < dirs.size(); ++j) nearest = std::min(nearest, arc(dirs[i], dirs[j]));
    }
    CHECK(nearest > 40.0 * kDeg2Rad);  // twelve, evenly: none crowds another
    CHECK(length(sum) < 0.5);
}

TEST_CASE("copies of one reflection land within 3 ms of each other's average, and keep their order") {
    const Room room = hall();
    auto bus = std::make_unique<BusReflections>();
    busReflections(room, 6.0, *bus);  // 2 x 6 / 343 = 35 ms apart before
    REQUIRE(bus->count == 62);
    auto raw = std::make_unique<std::array<Reflection, kMaxImages>>();
    double widest = 0.0;
    for (int i = 0; i < 62; ++i) {
        double mean = 0.0, rawMean = 0.0;
        for (int v = 0; v < kVirtualSources; ++v)
            mean += bus->taps[static_cast<std::size_t>(v)][static_cast<std::size_t>(i)].delay;
        mean /= kVirtualSources;
        for (int v = 0; v < kVirtualSources; ++v) {
            widest = std::max(
                widest, std::abs(bus->taps[static_cast<std::size_t>(v)][static_cast<std::size_t>(i)].delay - mean));
            reflectionsOf(room, bus->direction[static_cast<std::size_t>(v)], 6.0, *raw);
            rawMean += (*raw)[static_cast<std::size_t>(i)].delay;
        }
        CHECK(mean == doctest::Approx(rawMean / kVirtualSources));  // pulled toward their average, which stays
    }
    CHECK(widest <= kSpreadSeconds + 1e-12);
    CHECK(widest > 0.5 * kSpreadSeconds);  // and the spread is used, not collapsed: at 0 they comb

    //  a source so near that the copies are within 3 ms already is left alone
    busReflections(room, 0.3, *bus);
    reflectionsOf(room, bus->direction[2], 0.3, *raw);
    for (int i = 0; i < 62; ++i)
        CHECK(bus->taps[2][static_cast<std::size_t>(i)].delay ==
              doctest::Approx((*raw)[static_cast<std::size_t>(i)].delay));
}
