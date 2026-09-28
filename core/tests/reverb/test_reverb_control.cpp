// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/patch/state.hpp"
#include "bambi/reverb/control.hpp"
#include "bambi/reverb/params.hpp"
#include "doctest.h"

using namespace bambi;

/*  From Reverb's settings to the frame its engine plays. The one place the control units --
 *  metres, seconds, decibels, degrees, hertz -- become the engine's.
 */

TEST_CASE("the control step is the one place the units change") {
    /*  Catches: milliseconds passed through as seconds (200 ms of trim becoming 200 s, which the
        engine then clamps to its own maximum and hides), or degrees reaching the engine unturned. */
    ReverbSettings s;
    s.preDelayTrimMs = 40.0;
    s.send.shape.kind = RegionKind::Spot;
    s.send.deg.size = 90.0;  // degrees at the boundary
    s.send.deg.softness = 30.0;

    const ReverbFrame f = resolveReverb(s, false);
    CHECK(f.preDelayTrimSeconds == doctest::Approx(0.040));
    CHECK(f.send.size == doctest::Approx(90.0 * kDeg2Rad));
    CHECK(f.send.softness == doctest::Approx(30.0 * kDeg2Rad));
    CHECK(f.send.kind == RegionKind::Spot);

    //  decibels are the engine's own unit and are not converted here
    s.wetDb = -6.0;
    CHECK(resolveReverb(s, false).levelDb == doctest::Approx(-6.0));
}

TEST_CASE("every control is held to its range, so a bad setting cannot reach the engine") {
    /*  Catches: any clamp removed. A size of zero is a box with no volume, a decay of zero divides
        by it, and a pre-delay of a minute is not a trim. The engine trusts its frame. */
    ReverbSettings s;
    s.sizeMetres = -5.0;
    s.decaySeconds = 0.0;
    s.tone = 4.0;
    s.roughness = -1.0;
    s.distanceMetres = 1e6;
    s.wetDb = 400.0;
    s.preDelayTrimMs = 60000.0;
    s.lowCutHz = 1.0;
    s.highCutHz = 96000.0;
    s.send.amount = 5.0;
    s.returnTo.amount = -2.0;

    const ReverbFrame f = resolveReverb(s, false);
    CHECK(f.room.size >= 3.0);
    CHECK(f.room.decay >= 0.2);
    CHECK(f.room.tone <= 1.0);
    CHECK(f.room.roughness >= 0.0);
    CHECK(f.distance <= 20.0);
    CHECK(f.levelDb <= 12.0);
    CHECK(f.preDelayTrimSeconds <= 0.2);
    CHECK(f.lowCutHz >= 20.0);
    CHECK(f.highCutHz <= 20000.0);
    CHECK(f.sendAmount == doctest::Approx(1.0));
    CHECK(f.returnAmount == doctest::Approx(0.0));

    //  and the pre-delay only ever adds: the derived one is the first wall's arrival already
    ReverbSettings back;
    back.preDelayTrimMs = -50.0;
    CHECK(resolveReverb(back, false).preDelayTrimSeconds == doctest::Approx(0.0));
}

TEST_CASE("quality is the line count and nothing else, and a render may take the better one") {
    /*  Efficient or realistic while playing, and whether a render switches. Measured,
        the line count is the lever and the tail's order is not -- 16 lines to 8 takes the hall from
        4.76 % of a core to 3.30 %, where dropping the tail to order 1 saves 0.05 %.

        Catches: `offline` ignored (a bounce plays what the meter plays, so the switch does
        nothing), `renderQuality` ignored, and quality reaching tailOrder instead of tailLines. */
    CHECK(tailLinesFor(ReverbQuality::Efficient) == 8);
    CHECK(tailLinesFor(ReverbQuality::Realistic) == 16);

    ReverbSettings light;
    light.quality = ReverbQuality::Efficient;

    light.renderQuality = RenderQuality::Same;
    CHECK(resolveReverb(light, false).tailLines == 8);
    CHECK(resolveReverb(light, true).tailLines == 8);  // asked to follow, so it follows

    light.renderQuality = RenderQuality::Realistic;
    CHECK(resolveReverb(light, false).tailLines == 8);  // still light while playing
    CHECK(resolveReverb(light, true).tailLines == 16);  // and full when bouncing

    //  the tail's order is not a quality lever, and is the same whatever is asked for
    CHECK(resolveReverb(light, false).tailOrder == resolveReverb(light, true).tailOrder);
}

TEST_CASE("there are six presets, and each sets the room without touching anything else") {
    /*  Catches: a preset writing the level, the regions or the quality. A preset is a starting
        point: taking one should not silently undo a send the user has already aimed. */
    struct Want {
        ReverbPreset p;
        double size;
        RoomShape shape;
        double decay, distance;
    };
    const Want kWant[] = {{ReverbPreset::Ambience, 4.0, RoomShape::Room, 0.35, 1.5},
                          {ReverbPreset::Room, 5.0, RoomShape::Room, 0.50, 2.0},
                          {ReverbPreset::Chamber, 7.0, RoomShape::Tall, 1.20, 3.0},
                          {ReverbPreset::Hall, 20.0, RoomShape::Hall, 1.90, 6.0},
                          {ReverbPreset::LargeHall, 28.0, RoomShape::Hall, 2.80, 10.0},
                          {ReverbPreset::Cathedral, 38.0, RoomShape::Tall, 6.50, 14.0}};
    for (const Want& w : kWant) {
        const ReverbSettings s = ReverbSettings::preset(w.p);
        CHECK(s.sizeMetres == doctest::Approx(w.size));
        CHECK(s.shape == w.shape);
        CHECK(s.decaySeconds == doctest::Approx(w.decay));
        CHECK(s.distanceMetres == doctest::Approx(w.distance));
        //  untouched by any preset
        CHECK(s.wetDb == doctest::Approx(0.0));
        CHECK(s.send.shape.kind == RegionKind::Everywhere);
        CHECK(s.returnTo.amount == doctest::Approx(1.0));
        CHECK(s.quality == ReverbQuality::Realistic);
    }
    //  the cuts are the preset's own, and differ between them
    CHECK(ReverbSettings::preset(ReverbPreset::Ambience).lowCutHz == doctest::Approx(180.0));
    CHECK(ReverbSettings::preset(ReverbPreset::Cathedral).lowCutHz == doctest::Approx(120.0));
}

TEST_CASE("a region's side and shape reach the frame, and the amounts are the slots' own") {
    /*  Catches: send and return crossed, or the side dropped -- both of which a level test cannot
        see, since an everywhere region at amount 1 is the same either way. */
    ReverbSettings s;
    s.send.shape.kind = RegionKind::Band;
    s.send.side = RegionSide::Outside;
    s.send.amount = 0.25;
    s.returnTo.shape.kind = RegionKind::Dots;
    s.returnTo.shape.dots = 12;
    s.returnTo.amount = 0.5;

    const ReverbFrame f = resolveReverb(s, false);
    CHECK(f.send.kind == RegionKind::Band);
    CHECK(f.send.side == RegionSide::Outside);
    CHECK(f.sendAmount == doctest::Approx(0.25));
    CHECK(f.returnRegion.kind == RegionKind::Dots);
    CHECK(f.returnRegion.dots == 12);
    CHECK(f.returnRegion.side == RegionSide::Inside);
    CHECK(f.returnAmount == doctest::Approx(0.5));

    //  a dot count that is not one of the solids is made one, as resolveRegion promises
    s.returnTo.shape.dots = 7;
    CHECK(resolveReverb(s, false).returnRegion.dots == 6);
}

TEST_CASE("a frame the engine can play: the default settings drive it without any other help") {
    //  Catches: a field left unset in resolveReverb, which shows here as a room that makes no sound.
    ReverbEngine engine;
    engine.prepare(3, 48000.0);
    engine.set(resolveReverb(ReverbSettings::preset(ReverbPreset::Hall), false));
    CHECK(engine.room().rtMid == doctest::Approx(1.9));
    CHECK(engine.lines() == 16);
    CHECK(engine.balance().roomEnergy > 0.0);
}

TEST_CASE("two levels, not a mix: the dry is an output stage and the room does not move") {
    /*  What a crossfade cannot say: keep the room exactly where it is and bring the source
        forward. So dry and wet are independent, and the dry is mixed in after everything -- the
        reflections, the tail and the balance between them are untouched, which is what keeps every
        measurement of what the room does still about the room.

        Catches: the dry folded into the wet (a mix), the dry reaching the engine before the
        reflections read the bus (it would echo itself), and -60 dB left as a gain rather than off. */
    ReverbEngine engine;
    engine.prepare(3, 48000.0);
    const int C = engine.channels(), frames = 4096;
    std::vector<float> in(static_cast<std::size_t>(frames) * C, 0.0f), out(in.size());
    for (int f = 0; f < frames; ++f) in[static_cast<std::size_t>(f) * C] = f == 0 ? 1.0f : 0.0f;

    const auto render = [&](double dryDb, double wetDb) {
        ReverbSettings s = ReverbSettings::preset(ReverbPreset::Hall);
        s.dryDb = dryDb;
        s.wetDb = wetDb;
        engine.reset();
        engine.set(resolveReverb(s, false));
        for (int at = 0; at < frames; at += kReverbHop)
            engine.process(in.data() + static_cast<std::size_t>(at) * C, out.data() + static_cast<std::size_t>(at) * C,
                           std::min(kReverbHop, frames - at));
        double dry = 0.0, room = 0.0;
        for (int f = 0; f < frames; ++f) {
            const double v = out[static_cast<std::size_t>(f) * C];
            (f == 0 ? dry : room) += v * v;
        }
        return std::pair<double, double>{dry, room};
    };

    //  dry off is what a send has always had: nothing at sample zero but the room's own first tap
    const auto [dryOff, roomOff] = render(-60.0, 0.0);
    CHECK(dryOff < 1.0e-6);

    /*  Bringing the dry up does not move the room by so much as a bit. A trim of 0 dB is the room as
        it really is, so the dry lands at 1/sqrt(K) -- at the hall's 6 m, K is about 1.70, so 0.77 of
        the input rather than all of it. */
    const auto [dryUp, roomUp] = render(0.0, 0.0);
    CHECK(dryUp == doctest::Approx(1.0 / 1.695).epsilon(0.05));
    CHECK(roomUp == doctest::Approx(roomOff).epsilon(1e-9));

    //  and the wet moves on its own: -6 dB is a quarter of the energy, with the dry unchanged
    const auto [dryHalf, roomHalf] = render(0.0, -6.0);
    CHECK(dryHalf == doctest::Approx(dryUp).epsilon(1e-9));
    CHECK(roomHalf == doctest::Approx(roomUp * 0.25).epsilon(0.05));

    //  -60 dB is silence exactly, not "very quiet"
    CHECK(resolveReverb(
              [] {
                  ReverbSettings s;
                  s.dryDb = -60.0;
                  return s;
              }(),
              false)
              .dryGain == 0.0);
    CHECK(resolveReverb(
              [] {
                  ReverbSettings s;
                  s.dryDb = -59.0;
                  return s;
              }(),
              false)
              .dryGain > 0.0);
}

TEST_CASE("a region's rate turns it, and a reset puts the turn back") {
    /*  Catches: drop either advance and the region stops turning; drop reset() and the second
        render differs from the first, which is the thing a bounce must not do. */
    ReverbSettings s;
    s.send.shape.kind = RegionKind::Spot;
    s.send.rates.yaw = 90.0;  // a quarter turn a second
    s.returnTo.shape.kind = RegionKind::Spot;

    ReverbResolver r;
    const double fs = 48000.0;
    const int hopsPerSecond = static_cast<int>(fs) / kReverbHop;

    const Region first = r.resolve(s, false, fs).send;
    Region last = first;
    for (int i = 1; i < hopsPerSecond; ++i) last = r.resolve(s, false, fs).send;
    //  one second at 90 deg/s: a quarter turn, which the accumulator gets to within its last hop
    CHECK(last.yaw - first.yaw == doctest::Approx(kPi / 2).epsilon(0.01));

    //  the return region has no rate of its own, so it stays put while the send one turns
    CHECK(r.resolve(s, false, fs).returnRegion.yaw == doctest::Approx(resolveReverb(s, false).returnRegion.yaw));

    r.reset();
    CHECK(r.resolve(s, false, fs).send.yaw == doctest::Approx(first.yaw));
}

TEST_CASE("a preset reaches the host's own parameters") {
    /*  Catches: drop any line from applyPreset and the setting it carries stops moving; write a
        raw value instead of a normalised one and every one of these lands somewhere else. */
    const ParamManifest& m = reverbParams();
    std::array<float, kMaxParams> values{};
    for (int i = 0; i < m.size(); ++i) values[static_cast<std::size_t>(i)] = toNormalised(m, i, m[i].def);

    RoomState room;
    applyPreset(
        m, ReverbPreset::Cathedral,
        [&](int at, float normalised) { values[static_cast<std::size_t>(at)] = normalised; }, room);
    CHECK(room.preset == static_cast<int>(ReverbPreset::Cathedral));
    const RegionShape spot{RegionKind::Spot, 4, 6};
    const auto got = reverbSettingsFrom(
        m,
        [&] {
            //  back out of normalised, the way a host hands a plugin its values
            std::array<float, kMaxParams> plain{};
            for (int i = 0; i < m.size(); ++i)
                plain[static_cast<std::size_t>(i)] = fromNormalised(m, i, values[static_cast<std::size_t>(i)]);
            return plain;
        }(),
        static_cast<RoomShape>(room.shape), spot, spot, RenderQuality::Realistic);

    const auto wanted = ReverbSettings::preset(ReverbPreset::Cathedral);
    CHECK(got.sizeMetres == doctest::Approx(wanted.sizeMetres).epsilon(0.01));
    CHECK(got.decaySeconds == doctest::Approx(wanted.decaySeconds).epsilon(0.01));
    CHECK(got.tone == doctest::Approx(wanted.tone).epsilon(0.01));
    CHECK(got.roughness == doctest::Approx(wanted.roughness).epsilon(0.01));
    CHECK(got.distanceMetres == doctest::Approx(wanted.distanceMetres).epsilon(0.01));
    CHECK(got.lowCutHz == doctest::Approx(wanted.lowCutHz).epsilon(0.02));
    CHECK(got.highCutHz == doctest::Approx(wanted.highCutHz).epsilon(0.02));
    CHECK(got.shape == wanted.shape);

    SUBCASE("and leaves alone what the table does not list") {
        //  The levels, the regions and the quality are the user's, not the preset's.
        CHECK(got.wetDb == doctest::Approx(0.0));
        CHECK(got.dryDb == doctest::Approx(0.0));  // where dry opens
    }
}

/*  As Echo's, for both slots: catches the hold being dropped, `continue` not running while stopped,
 *  and a restart that clears a turn the user asked to keep -- or keeps one a render needs cleared. */
TEST_CASE("both regions' turns are held, restarted or left running as rates.retrigger says") {
    ReverbSettings s;
    s.send.rates.yaw = 90.0;
    s.returnTo.rates.yaw = -45.0;

    ReverbResolver r;
    for (int i = 0; i < 100; ++i) r.resolve(s, false, 48000.0, false);
    CHECK(r.sendTurn().yawRad() == doctest::Approx(0.0));
    CHECK(r.returnTurn().yawRad() == doctest::Approx(0.0));

    for (int i = 0; i < 100; ++i) r.resolve(s, false, 48000.0, true);
    const double send = r.sendTurn().yawRad(), back = r.returnTurn().yawRad();
    CHECK(send > 0.1);
    CHECK(back < -0.05);
    r.restartTransport(s.ratesContinue);
    CHECK(r.sendTurn().yawRad() == doctest::Approx(0.0));
    CHECK(r.returnTurn().yawRad() == doctest::Approx(0.0));

    s.ratesContinue = true;
    for (int i = 0; i < 100; ++i) r.resolve(s, false, 48000.0, false);
    CHECK(r.sendTurn().yawRad() == doctest::Approx(send));
    r.restartTransport(s.ratesContinue);
    CHECK(r.sendTurn().yawRad() == doctest::Approx(send));
    CHECK(r.returnTurn().yawRad() == doctest::Approx(back));
}

TEST_CASE("a fresh instance's room state is the hall its parameters open as") {
    //  RoomState lives below reverb/ and stores the preset and shape as numbers.
    //  Catches: either number drifting from the enums it stands for.
    const RoomState room;
    CHECK(room.preset == static_cast<int>(ReverbPreset::Hall));
    CHECK(room.shape == static_cast<int>(RoomShape::Hall));
    CHECK(kRoomNames[static_cast<std::size_t>(room.preset)] == "hall");
}
