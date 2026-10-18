// SPDX-License-Identifier: GPL-3.0-or-later
#include <vector>

#include "bambi/live/midi.hpp"
#include "doctest.h"

using namespace bambi::live;

TEST_CASE("MIDI waits for the piece it falls in, taken in sample order, the rest kept") {
    //  Catches: an event taken before its time, one lost when others are taken, events out of order, a full queue
    //  that overwrites rather than drops.
    MidiPending p;
    p.add({300, {0x90, 60, 100}, 3});
    p.add({100, {0x90, 62, 100}, 3});
    p.add({200, {0x80, 62, 0}, 3});
    MidiEvent out[8];
    CHECK(p.take(250, out, 8) == 2);
    CHECK(out[0].sample == 100);
    CHECK(out[1].sample == 200);
    CHECK(p.size() == 1);
    CHECK(p.take(1000, out, 8) == 1);
    CHECK(out[0].bytes[1] == 60);
    for (int i = 0; i < MidiPending::kCapacity; ++i) CHECK(p.add({i, {0x90, 1, 1}, 3}));
    CHECK_FALSE(p.add({5, {0x90, 2, 1}, 3}));
    CHECK(p.take(2, out, 8) == 2);
}

TEST_CASE("the lead of Live's MIDI is the middle of the last measurements, and says how far they agree") {
    //  Catches: an outlier taken for the lead, a spread that ignores disagreement, the oldest never let go.
    MidiLead lead;
    CHECK(lead.median() == 0.0);
    for (const double ms : {11.0, 12.0, 11.5, 40.0, 11.8}) lead.add(ms);
    CHECK(lead.count() == 5);
    CHECK(lead.median() == 11.8);
    CHECK(lead.spread() < 1.0);
    for (int i = 0; i < MidiLead::kKept; ++i) lead.add(20.0);
    CHECK(lead.median() == 20.0);
    CHECK(lead.count() == MidiLead::kKept);
}

TEST_CASE("a note is paired with the click it sets off, after it or just before, and nothing else is") {
    //  Catches: an onset inside a sound taken for a new one, a click past the window paired, a lead sign flipped
    //  when the sound came first, one result given twice.
    LeadMeter m(48000.0);
    std::vector<float> quiet(4800, 0.0f), click(4800, 0.0f);
    for (int i = 0; i < 100; ++i) click[static_cast<std::size_t>(i)] = 0.5f;
    m.listen(quiet.data(), nullptr, 4800, 0);
    m.note(4800);
    m.listen(quiet.data(), quiet.data(), 480, 4800);
    m.listen(click.data(), nullptr, 4800, 5280);
    CHECK(m.took() == std::optional<std::int64_t>{480});
    CHECK_FALSE(m.took().has_value());
    m.listen(click.data(), nullptr, 4800, 10080);  // the click again, no note: an onset kept for the next note
    m.note(10080 + 96);
    CHECK(m.took() == std::optional<std::int64_t>{-96});
    m.note(20000);
    m.listen(quiet.data(), nullptr, 4800, 14880);
    m.listen(quiet.data(), nullptr, 4800, 19680);
    m.listen(quiet.data(), nullptr, 4800, 24480);
    m.listen(quiet.data(), nullptr, 4800, 29280);
    m.listen(quiet.data(), nullptr, 4800, 34080);
    m.listen(click.data(), nullptr, 4800, 38880);  // past 300 ms
    CHECK_FALSE(m.took().has_value());
}
