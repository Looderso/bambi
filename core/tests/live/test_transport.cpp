// SPDX-License-Identifier: GPL-3.0-or-later
//
//  Live's timeline as the host's engines read it: messages in, a playhead out.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "bambi/live/arrival.hpp"
#include "bambi/live/transport.hpp"
#include "doctest.h"

using namespace bambi::live;

namespace {

constexpr double kRate = 48000.0;

ProjectorSettings settings() {
    ProjectorSettings s;
    s.sampleRate = kRate;
    s.slackSamples = 2400;
    s.catchUpSamples = 24000;
    s.maxSkew = 0.01;
    return s;
}

}  // namespace

TEST_CASE("playing at a tempo, the position is the closed form from the message") {
    //  Catches: tempo taken per millisecond or per minute where per second is meant, or the rate left out.
    TransportProjector p(settings());
    p.receive({true, 4.0, 120.0}, 1000);
    const Playhead h = p.reading(1000 + 48000);  // one second at 120 bpm: two quarter notes
    CHECK(h.playing);
    CHECK(h.ppq == doctest::Approx(6.0).epsilon(1e-12));
    CHECK(h.bpm == 120.0);
    CHECK(h.timeline == 96000 + 48000);  // 4 quarter notes at 120 bpm is 2 s, then one more second
}

TEST_CASE("readings do not depend on how often they are asked for") {
    //  Catches: state advanced per call -- an integrated position, a floor moved by the block -- which would
    //  make a render's movement depend on the host's buffer size.
    struct Timed {
        TransportMessage m;
        std::int64_t at;
    };
    const std::vector<Timed> messages{
        {{true, 0.0, 120.0}, 0},
        {{true, 2.01, 120.0}, 48000},  // inside the slack: caught up
        {{true, 4.0, 90.0}, 96000},    // a new tempo
        {{true, 32.0, 90.0}, 144000},  // a locate
    };
    const auto run = [&](std::int64_t block) {
        TransportProjector p(settings());
        std::vector<Playhead> at;
        std::size_t next = 0;
        for (std::int64_t s = 0; s < 4 * 48000; s += block) {
            //  a message that arrived during the last block is handed over before this one, at its own sample
            while (next < messages.size() && messages[next].at <= s) {
                p.receive(messages[next].m, messages[next].at);
                ++next;
            }
            const Playhead h = p.reading(s);
            if (s % 1024 == 0) at.push_back(h);
        }
        return at;
    };
    const std::vector<Playhead> reference = run(1024);
    for (const std::int64_t block : {1, 2, 64, 128, 512, 1024}) {
        const std::vector<Playhead> got = run(block);
        REQUIRE(got.size() == reference.size());
        for (std::size_t i = 0; i < got.size(); ++i) {
            CHECK(got[i].ppq == reference[i].ppq);
            CHECK(got[i].timeline == reference[i].timeline);
        }
    }
}

TEST_CASE("a late message inside the slack moves neither the timeline nor the position back") {
    //  Catches: re-anchoring on every message, which turns each bit of network jitter into a locate the
    //  engines reset on, a position that steps backwards, or a message stamped before the last reading taking
    //  effect from before it.
    TransportProjector p(settings());
    p.receive({true, 0.0, 120.0}, 0);
    const Playhead before = p.reading(48000);
    p.receive({true, 1.97, 120.0}, 47520);  // 10 ms behind, stamped before the read at 48000: late, not a locate
    const Playhead after = p.reading(48000 + 64);
    CHECK(after.timeline == before.timeline + 64);
    CHECK(after.ppq >= before.ppq);
    CHECK(after.ppq < before.ppq + 64 * 120.0 / (60.0 * kRate));  // and runs a little slow to close it
    CHECK(p.reading(48000).ppq == before.ppq);                    // from what was read, not back before it
}

TEST_CASE("past the slack it is a locate: the timeline jumps once and runs on from there") {
    //  Catches: a slack compared in the wrong unit, so a real locate is slewed through for seconds.
    TransportProjector p(settings());
    p.receive({true, 0.0, 120.0}, 0);
    (void)p.reading(48000);
    p.receive({true, 16.0, 120.0}, 48000);
    const Playhead jumped = p.reading(48000);
    CHECK(jumped.ppq == doctest::Approx(16.0));
    CHECK(jumped.timeline == 16 * 24000);
    const Playhead next = p.reading(48000 + 512);
    CHECK(next.timeline == jumped.timeline + 512);
}

TEST_CASE("a new tempo takes effect from the message's sample, not back to the last anchor") {
    //  Catches: rescaling from the old anchor, which would move every position since the last message.
    TransportProjector p(settings());
    p.receive({true, 0.0, 120.0}, 0);
    p.receive({true, 8.0, 60.0}, 192000);  // exactly on the projection, then half as fast
    const Playhead h = p.reading(192000 + 48000);
    CHECK(h.bpm == 60.0);
    CHECK(h.ppq == doctest::Approx(9.0).epsilon(1e-12));
    CHECK(h.timeline == 192000 + 48000);  // a tempo change is not a locate
}

TEST_CASE("stopped, the position holds and the timeline waits; started again, it starts from Live") {
    //  Catches: a projection that runs on while stopped, or a start that carries on from the old timeline.
    TransportProjector p(settings());
    p.receive({true, 0.0, 120.0}, 0);
    p.receive({false, 2.0, 120.0}, 48000);
    const Playhead a = p.reading(48000 + 1000);
    const Playhead b = p.reading(48000 + 50000);
    CHECK_FALSE(a.playing);
    CHECK(a.ppq == 2.0);
    CHECK(b.ppq == 2.0);
    CHECK(a.timeline == b.timeline);

    p.receive({true, 8.0, 120.0}, 200000);
    const Playhead c = p.reading(200000);
    CHECK(c.playing);
    CHECK(c.ppq == doctest::Approx(8.0));
    CHECK(c.timeline == 8 * 24000);
}

TEST_CASE("a clock that drifts against the host's is followed without a locate") {
    //  Catches: differences inside the slack ignored rather than closed -- 500 ppm builds past the slack in
    //  under two minutes and the engines would reset in the middle of playback.
    TransportProjector p(settings());
    const double drift = 1.0005;  // Live's clock runs 500 ppm fast
    const double perSample = 120.0 / (60.0 * kRate);
    std::int64_t expected = 0;
    double worst = 0.0;
    p.receive({true, 0.0, 120.0}, 0);
    for (std::int64_t s = 0; s <= 120 * 48000; s += 512) {
        if (s % 4800 == 0 && s > 0) p.receive({true, static_cast<double>(s) * perSample * drift, 120.0}, s);
        const Playhead h = p.reading(s);
        CHECK(h.timeline == expected);  // never jumps
        expected += 512;
        worst = std::max(worst, std::abs(h.ppq - static_cast<double>(s) * perSample * drift));
    }
    CHECK(worst < 2400 * perSample);  // stays inside the slack the whole two minutes
}

TEST_CASE("a gap inside the slack is closed no faster than the skew allows") {
    //  Catches: an unbounded catch-up, which with a short catch-up time races the position ahead -- or, a gap
    //  behind, runs it backwards.
    TransportProjector p(settings());
    const double perSample = 120.0 / (60.0 * kRate);
    p.receive({true, 0.0, 120.0}, 0);
    p.receive({true, 2000 * perSample, 120.0}, 0 + 1);  // 2000 samples ahead, just inside the slack
    const Playhead a = p.reading(1);
    const Playhead b = p.reading(1 + 4800);
    CHECK(b.ppq - a.ppq <= 4800 * perSample * (1.0 + 0.01) + 1e-12);
    CHECK(b.ppq - a.ppq > 4800 * perSample);
}

TEST_CASE("before any message the playhead is stopped at zero") {
    //  Catches: a reading made up from defaults that claims to be playing.
    TransportProjector p(settings());
    const Playhead h = p.reading(123456);
    CHECK_FALSE(h.playing);
    CHECK(h.ppq == 0.0);
    CHECK(h.timeline == 0);
}

namespace {

/*  Live playing at 120 bpm for ten seconds, its position read by the device every 20 ms and each message held back
 *  60 to 200 ms on its way, in order, as a busy queue holds it -- fed through the host's own path: the stamp turned into the
 *  steady clock, the arrival into a sample, the projector. Returns how many locates the projector decided. */
std::uint32_t locatesWithLateMessages(bool stamped) {
    constexpr double rate = 48000.0, bpm = 120.0, epochMs = 1.7e12;  // the system clock, this far ahead of the steady
    TransportProjector projector({rate});
    Arrivals arrivals(rate);
    struct Sent {
        std::int64_t readNs, arrivesNs;
        TransportMessage message;
    };
    std::vector<Sent> sent;
    std::uint32_t seed = 7;
    std::int64_t lastArrival = 0;
    for (std::int64_t readNs = 0; readNs < 10'000'000'000; readNs += 20'000'000) {
        seed = seed * 1664525u + 1013904223u;
        const std::int64_t held = 60'000'000 + static_cast<std::int64_t>(seed % 140'000'000u);
        //  one connection, in order: a message held up holds up the ones behind it
        lastArrival = std::max(lastArrival, readNs + held);
        const double ppq = bpm / 60.0 * 1e-9 * static_cast<double>(readNs);
        sent.push_back(
            {readNs, lastArrival, {true, ppq, bpm, stamped ? epochMs + 1e-6 * static_cast<double>(readNs) : 0.0}});
    }
    std::size_t next = 0;
    for (std::int64_t block = 0; block < static_cast<std::int64_t>(11 * rate); block += 128) {
        const auto nowNs = static_cast<std::int64_t>(std::llround(1e9 * static_cast<double>(block) / rate));
        for (; next < sent.size() && sent[next].arrivesNs <= nowNs; ++next) {
            const Sent& m = sent[next];
            const auto at = readAtNs(m.arrivesNs, nowNs, epochMs + 1e-6 * static_cast<double>(nowNs), m.message.readMs);
            projector.receive(m.message, arrivals.at(block, 1e-9 * static_cast<double>(nowNs - at)));
        }
        (void)projector.reading(block);
    }
    return projector.locates();
}

}  // namespace

TEST_CASE("a message held back on its way is not a locate, when it says when Live's position was read") {
    //  Catches: the host placing a message at its arrival rather than at the device's read, so that a busy queue
    //  between them reads as Live jumping back and every rate resets. Without stamps the same messages must make
    //  locates, or the scene does not hold them back far enough to test anything.
    CHECK(locatesWithLateMessages(true) == 0);
    CHECK(locatesWithLateMessages(false) > 0);
}

TEST_CASE("a device's stamp is believed only between the arrival and a few seconds before it") {
    //  Catches: a stamp from after the arrival, or from a clock that stepped, moving a message anywhere at all.
    const std::int64_t arrived = 5'000'000'000, now = 5'010'000'000;
    const double nowMs = 1000.0 + 1e-6 * static_cast<double>(now);
    CHECK(readAtNs(arrived, now, nowMs, 0.0) == arrived);                              // no stamp
    CHECK(readAtNs(arrived, now, nowMs, nowMs - 100.0) == now - 100'000'000);          // read 100 ms ago
    CHECK(readAtNs(arrived, now, nowMs, nowMs - 1.0) == arrived);                      // after it arrived
    CHECK(readAtNs(arrived, now, nowMs, nowMs - (kStampTrustMs + 100.0)) == arrived);  // too old to believe
}
