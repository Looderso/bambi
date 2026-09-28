// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "bambi/echo/control.hpp"
#include "bambi/echo/params.hpp"
#include "bambi/patch/readparams.hpp"
#include "bambi/reverb/control.hpp"
#include "bambi/reverb/params.hpp"
#include "doctest.h"

using namespace bambi;

/*  Parameters to settings. This is the seam between a plugin's key list and its control
 *  layer, and the fault it exists to catch is a key that nothing reads: a control the user moves
 *  that does nothing at all, which no engine test can see because the engine is never told.
 */
namespace {

/*  A frame as a list of numbers.

    Not `memcmp`. Two frames built the ordinary way differ in their padding, which is never
    initialised, so a byte comparison reports a difference every time and a test built on it passes
    whatever the code does. Zeroing first does not help either: assigning a trivially copyable type
    copies the whole object representation, padding and all, straight back over the zeroes. Writing
    the fields out is the only comparison that means what it says.

    Every field a control can reach is here; `reached` below counts how many keys actually moved one,
    so a field left out of this list shows up as a key that changes nothing. */
std::vector<double> digest(const EchoFrame& f) {
    std::vector<double> d;
    const auto push = [&d](double v) { d.push_back(v); };
    for (const EchoTapFrame& t : f.taps) {
        push(t.on ? 1 : 0);
        push(t.periodSamples);
        push(t.offsetSamples);
        push(t.level);
        push(t.feedback);
        push(t.pass.axis.x);
        push(t.pass.axis.y);
        push(t.pass.axis.z);
        push(t.pass.axisIsPole ? 1 : 0);
        push(t.pass.skew);
        push(t.pass.spinRad);
        push(t.pass.blurRad);
        push(t.pass.lowCutHz);
        push(t.pass.highCutHz);
    }
    push(static_cast<double>(f.send.kind));
    push(static_cast<double>(f.send.side));
    push(f.send.yaw);
    push(f.send.pitch);
    push(f.send.roll);
    push(f.send.softness);
    push(f.send.size);
    push(f.send.bandElevation);
    push(f.send.thickness);
    push(f.send.sectors);
    push(f.send.fill);
    push(f.send.dots);
    push(f.send.dotSize);
    push(f.send.coverage);
    push(f.send.contrast);
    push(f.send.detail);
    push(f.send.evolve);
    push(f.sendAmount);
    push(f.wetGain);
    push(f.dryGain);
    return d;
}

std::vector<double> digest(const ReverbFrame& f) {
    std::vector<double> d;
    const auto push = [&d](double v) { d.push_back(v); };
    push(f.room.size);
    push(static_cast<double>(f.room.shape));
    push(f.room.decay);
    push(f.room.tone);
    push(f.room.roughness);
    push(f.distance);
    push(f.levelDb);
    push(f.dryGain);
    push(f.preDelayTrimSeconds);
    push(f.lowCutHz);
    push(f.highCutHz);
    push(f.tailLines);
    push(f.tailOrder);
    push(f.drift ? 1 : 0);
    for (const Region* r : {&f.send, &f.returnRegion}) {
        push(static_cast<double>(r->kind));
        push(static_cast<double>(r->side));
        push(r->yaw);
        push(r->pitch);
        push(r->roll);
        push(r->softness);
        push(r->size);
        push(r->bandElevation);
        push(r->thickness);
        push(r->sectors);
        push(r->fill);
        push(r->dots);
        push(r->dotSize);
        push(r->coverage);
        push(r->contrast);
        push(r->detail);
        push(r->evolve);
    }
    push(f.sendAmount);
    push(f.returnAmount);
    return d;
}

/// Somewhere this parameter is not now, inside its range.
float juceLikeAway(float from, float min, float max) { return std::abs(from - max) > std::abs(from - min) ? max : min; }

/// Every parameter at its default, as a host would hold them.
std::array<float, kMaxParams> defaults(const ParamManifest& m) {
    std::array<float, kMaxParams> v{};
    for (int i = 0; i < m.size(); ++i) v[static_cast<std::size_t>(i)] = m[i].def;
    return v;
}

}  // namespace

TEST_CASE("every key Echo declares reaches its settings") {
    /*  Moved one at a time, away from its default, and the resolved frame must differ. A key that
        changes nothing is a control that does nothing -- the one fault a key list cannot show on its
        own, since the list is only names.

        The exceptions are named rather than skipped silently: a tap that is OFF hears none of its own
        settings, so the sweep turns every tap on first. */
    const ParamManifest& m = echoParams();
    auto base = defaults(m);
    /*  Two bases, synced and free. Both timing rows stay live with the inactive pair inert, so
        `steps` reaches nothing while a tap is free and `ms` reaches nothing while it is synced --
        and a key counts as reached if it moves the frame in either base, which is what that
        rule actually means. The taps are left at their own on/off defaults, since forcing them on is
        what makes "turn it on" a move to where it already was. */
    auto freeBase = base;
    for (int k = 1; k <= kEchoTaps; ++k)
        freeBase[static_cast<std::size_t>(m.byKey("tap" + std::to_string(k) + ".synced"))] = 0.0f;

    //  Not at 120 bpm: a tap's free time opens at its synced time at 120, where switching sync
    //  rightly moves nothing.
    const RegionShape spot{RegionKind::Spot, 4, 6};
    const auto echoBytes = [&](const std::array<float, kMaxParams>& v) {
        EchoResolver r;
        return digest(r.resolve(echoSettingsFrom(m, v, spot), 100.0, 48000.0, 3));
    };
    const auto beforeSynced = echoBytes(base);
    const auto beforeFree = echoBytes(freeBase);

    int reached = 0, skipped = 0;
    for (int at = 0; at < m.size(); ++at) {
        const ParamDesc& d = m[at];
        //  the generators and the matrix's amounts are the shared engine's, not the tap engine's:
        //  they reach a frame only once modulation is wired, which is not this seam's business
        const bool shared = d.key.starts_with("lfo") || d.key.starts_with("env") ||
                            (d.key.starts_with("mod.") && d.key != "mod.amount.region1")
                            //  a detector's release reaches the detector, which is before this seam:
                            //  the plugin's check proves it arrives
                            || d.key == "level.release" || d.key == "sc_level.release";
        if (shared) {
            ++skipped;
            continue;
        }

        //  away from where the base has it, not from the descriptor's default: moving a value to
        //  where it already is proves nothing
        const auto reaches = [&](const std::array<float, kMaxParams>& from, const std::vector<double>& was) {
            auto moved = from;
            const float now = from[static_cast<std::size_t>(at)];
            moved[static_cast<std::size_t>(at)] = juceLikeAway(now, d.min, d.max);
            return echoBytes(moved) != was;
        };

        INFO(d.key);
        bool differs = reaches(base, beforeSynced) || reaches(freeBase, beforeFree);
        /*  `rates.retrigger` decides what a turn does while the transport is stopped, so that is
            where it reaches the frame: a region with a rate, not playing. Restart holds it; continue
            turns it. Proven there rather than skipped. */
        if (d.key == "rates.retrigger") {
            auto turning = base;
            turning[static_cast<std::size_t>(m.byKey("region1.yaw_rate"))] = 90.0f;
            const auto stopped = [&](const std::array<float, kMaxParams>& v) {
                EchoResolver r;
                return digest(r.resolve(echoSettingsFrom(m, v, spot), 120.0, 48000.0, 3, false));
            };
            auto moved = turning;
            moved[static_cast<std::size_t>(at)] = 1.0f;
            differs = stopped(moved) != stopped(turning);
        }
        CHECK(differs);
        if (differs) ++reached;
    }
    INFO("reached ", reached, ", shared ", skipped);
    CHECK(reached + skipped == m.size());
    CHECK(reached >= 60);  // the four taps, rates included
}

TEST_CASE("every key Reverb declares reaches its settings") {
    //  As Echo's. Catches: any line dropped from reverbSettingsFrom -- that key stops moving the
    //  frame and is named here.
    const ParamManifest& m = reverbParams();
    const auto base = defaults(m);
    const RegionShape spot{RegionKind::Spot, 4, 6}, band{RegionKind::Band, 4, 6};
    /*  Through the resolver, not the bare function: a region's rates turn it over time, and only
        something holding the turn lets a rate reach the frame at all. */
    const auto frameOf = [&](const std::array<float, kMaxParams>& v) {
        ReverbResolver r;
        return digest(
            r.resolve(reverbSettingsFrom(m, v, RoomShape::Hall, spot, band, RenderQuality::Realistic), false, 48000.0));
    };
    const auto before = frameOf(base);

    int reached = 0, skipped = 0;
    for (int at = 0; at < m.size(); ++at) {
        const ParamDesc& d = m[at];
        const bool shared = d.key.starts_with("lfo") || d.key.starts_with("env") ||
                            (d.key.starts_with("mod.") && !d.key.starts_with("mod.amount.region")) ||
                            d.key == "level.release" || d.key == "sc_level.release";  // as Echo's
        if (shared) {
            ++skipped;
            continue;
        }

        auto moved = base;
        moved[static_cast<std::size_t>(at)] = juceLikeAway(base[static_cast<std::size_t>(at)], d.min, d.max);
        INFO(d.key);
        if (d.key == "rates.retrigger") {
            //  as Echo's: it reaches the frame where it decides something -- a rate, and not playing
            auto turning = base;
            turning[static_cast<std::size_t>(m.byKey("region1.yaw_rate"))] = 90.0f;
            const auto stopped = [&](const std::array<float, kMaxParams>& v) {
                ReverbResolver r;
                return digest(r.resolve(reverbSettingsFrom(m, v, RoomShape::Hall, spot, band, RenderQuality::Realistic),
                                        false, 48000.0, false));
            };
            moved = turning;
            moved[static_cast<std::size_t>(at)] = 1.0f;
            CHECK(stopped(moved) != stopped(turning));
        } else {
            CHECK(frameOf(moved) != before);
        }
        ++reached;
    }
    CHECK(reached + skipped == m.size());
    CHECK(reached >= 11 + 20);  // the room, output, input, quality and the two region blocks
}

TEST_CASE("a key a plugin does not have reads as the fallback, not as zero") {
    /*  One control layer serves a list that is still growing. Catches: the fallback dropped and a
        missing key reads 0 -- a decay of zero seconds, a size of zero metres, a room that divides by
        itself. */
    const ParamManifest& m = echoParams();  // has no room.decay
    const auto v = defaults(m);
    CHECK(readParam(m, v, "room.decay", 1.9) == doctest::Approx(1.9));
    CHECK(readChoice(m, v, "quality.playing", 1) == 1);
    CHECK(readParam(m, v, "tap1.level", 999.0) != doctest::Approx(999.0));  // it does have this one

    //  and a region block a plugin lacks reads the block's own defaults, so the region is evaluable
    const RegionSettingsDeg absent = readRegionSettings(m, v, "region2");
    const RegionSettingsDeg d;
    CHECK(absent.size == doctest::Approx(d.size));
    CHECK(absent.softness == doctest::Approx(d.softness));
    CHECK(readRegionSide(m, v, "region2") == RegionSide::Inside);
}

TEST_CASE("a fresh instance's parameters are the opening state each page documents") {
    /*  The key list's defaults and the control layer's `defaults()` are two statements of the same
        thing, written in different files, and nothing makes them agree: a tap block stamping one set
        of defaults across every tap would open all four the same way, rather than "tap 1 on, the
        rest off".

        Catches: any per-tap default changed in the macro, or `defaults()` changed without it. */
    const ParamManifest& m = echoParams();
    const auto v = defaults(m);
    const EchoSettings fromKeys = echoSettingsFrom(m, v, RegionShape{RegionKind::Everywhere, 4, 6});
    const EchoSettings written = EchoSettings::defaults();

    for (int k = 0; k < kEchoTaps; ++k) {
        const auto i = static_cast<std::size_t>(k);
        INFO("tap ", k + 1);
        CHECK(fromKeys.taps[i].on == written.taps[i].on);
        CHECK(fromKeys.taps[i].timing.synced == written.taps[i].timing.synced);
        CHECK(fromKeys.taps[i].timing.steps == written.taps[i].timing.steps);
        CHECK(fromKeys.taps[i].timing.offsetSteps == written.taps[i].timing.offsetSteps);
        CHECK(fromKeys.taps[i].timing.swing == doctest::Approx(written.taps[i].timing.swing));
        CHECK(fromKeys.taps[i].timing.ms == doctest::Approx(written.taps[i].timing.ms));
        CHECK(fromKeys.taps[i].timing.offsetMs == doctest::Approx(written.taps[i].timing.offsetMs));
        CHECK(fromKeys.taps[i].skew == doctest::Approx(written.taps[i].skew));
        CHECK(fromKeys.taps[i].lowCutHz == doctest::Approx(written.taps[i].lowCutHz));
        CHECK(fromKeys.taps[i].highCutHz == doctest::Approx(written.taps[i].highCutHz));
        //  a free time opens at the synced time at 120 bpm, so leaving sync moves nothing
        CHECK(written.taps[i].timing.ms == doctest::Approx(leaveSync(written.taps[i].timing, 120.0).ms));
        CHECK(written.taps[i].timing.offsetMs == doctest::Approx(leaveSync(written.taps[i].timing, 120.0).offsetMs));
        CHECK(fromKeys.taps[i].axisAzimuthDeg == doctest::Approx(written.taps[i].axisAzimuthDeg));
        CHECK(fromKeys.taps[i].axisElevationDeg == doctest::Approx(written.taps[i].axisElevationDeg));
        CHECK(fromKeys.taps[i].spinDeg == doctest::Approx(written.taps[i].spinDeg));
        CHECK(fromKeys.taps[i].levelDb == doctest::Approx(written.taps[i].levelDb));
        CHECK(fromKeys.taps[i].feedbackDb == doctest::Approx(written.taps[i].feedbackDb));
        CHECK(fromKeys.taps[i].blurDeg == doctest::Approx(written.taps[i].blurDeg));
    }
    //  Said outright rather than only compared: the first on, the rest waiting
    CHECK(fromKeys.taps[0].on);
    CHECK_FALSE(fromKeys.taps[1].on);
    CHECK_FALSE(fromKeys.taps[2].on);
    CHECK_FALSE(fromKeys.taps[3].on);
    CHECK(fromKeys.send.size == doctest::Approx(written.send.size));
    CHECK(fromKeys.send.softness == doctest::Approx(written.send.softness));
    CHECK(fromKeys.sendAmount == doctest::Approx(written.sendAmount));
    CHECK(fromKeys.wetDb == doctest::Approx(written.wetDb));
    CHECK(fromKeys.dryDb == doctest::Approx(written.dryDb));

    //  and Reverb's, against its own page's hall
    const ParamManifest& rm = reverbParams();
    const auto rv = defaults(rm);
    const ReverbSettings r =
        reverbSettingsFrom(rm, rv, RoomShape::Hall, RegionShape{}, RegionShape{}, RenderQuality::Realistic);
    const ReverbSettings hall = ReverbSettings::preset(ReverbPreset::Hall);
    CHECK(r.sizeMetres == doctest::Approx(hall.sizeMetres));
    CHECK(r.decaySeconds == doctest::Approx(hall.decaySeconds));
    CHECK(r.tone == doctest::Approx(hall.tone));
    CHECK(r.roughness == doctest::Approx(hall.roughness));
    CHECK(r.distanceMetres == doctest::Approx(hall.distanceMetres));
    CHECK(r.shape == hall.shape);
    CHECK(r.dryDb == doctest::Approx(0.0));  // dry opens at unity: an insert passes its input
}
