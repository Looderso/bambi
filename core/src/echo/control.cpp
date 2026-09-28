// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/echo/control.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>

#include "bambi/math/vec3.hpp"
#include "bambi/patch/readparams.hpp"

namespace bambi {
namespace {

constexpr ParamSpec kLevel = echoParamSpec("tap1", ".level");
constexpr ParamSpec kFeedback = echoParamSpec("tap1", ".feedback");
constexpr ParamSpec kAzimuth = echoParamSpec("tap1", ".az");
constexpr ParamSpec kElevation = echoParamSpec("tap1", ".el");
constexpr ParamSpec kSpin = echoParamSpec("tap1", ".spin");
constexpr ParamSpec kSkew = echoParamSpec("tap1", ".skew");
constexpr ParamSpec kBlur = echoParamSpec("tap1", ".blur");
constexpr ParamSpec kLowCut = echoParamSpec("tap1", ".low_cut");
constexpr ParamSpec kHighCut = echoParamSpec("tap1", ".high_cut");
constexpr ParamSpec kWet = echoParamSpec("output.wet");
constexpr ParamSpec kDry = echoParamSpec("output.dry");
constexpr ParamSpec kSendAmount = echoParamSpec("mod.amount.region1");

double gainOf(double db) { return std::pow(10.0, db / 20.0); }

/// `v` clamped to the parameter's range; NaN or infinity becomes `fallback`.
double held(double v, const ParamSpec& p, double fallback) {
    return std::isfinite(v) ? std::clamp(v, p.min, p.max) : fallback;
}
double held(double v, const ParamSpec& p) { return held(v, p, p.def); }

/// The bottom of the range is silence, not "very quiet".
double levelOf(double db, const ParamSpec& p) {
    const double clamped = std::clamp(db, p.min, p.max);
    return clamped <= p.min ? 0.0 : gainOf(clamped);
}

}  // namespace

EchoSettings echoSettingsFrom(const ParamManifest& m, std::span<const float> values, const RegionShape& sendShape) {
    EchoSettings s = EchoSettings::defaults();
    for (int k = 0; k < kEchoTaps; ++k) {
        //  Keys are built in a stack buffer: no allocation on a control step.
        std::array<char, kMaxKeyChars> buf{};
        char tap[8] = {'t', 'a', 'p', static_cast<char>('1' + k), '\0'};
        const std::string_view p{tap};
        EchoTapSettings& t = s.taps[static_cast<std::size_t>(k)];
        const EchoTapSettings d = t;  //  fallbacks for keys the manifest does not have
        t.on = readChoice(m, values, keyOf(buf, p, "on"), d.on ? 1 : 0) > 0;
        t.timing.synced = readChoice(m, values, keyOf(buf, p, "synced"), d.timing.synced ? 1 : 0) > 0;
        t.timing.steps = readChoice(m, values, keyOf(buf, p, "steps"), d.timing.steps);
        t.timing.offsetSteps = readChoice(m, values, keyOf(buf, p, "offset_steps"), d.timing.offsetSteps);
        t.timing.swing = readParam(m, values, keyOf(buf, p, "swing"), d.timing.swing);
        t.timing.ms = readParam(m, values, keyOf(buf, p, "ms"), d.timing.ms);
        t.timing.offsetMs = readParam(m, values, keyOf(buf, p, "offset_ms"), d.timing.offsetMs);
        t.levelDb = readParam(m, values, keyOf(buf, p, "level"), d.levelDb);
        t.feedbackDb = readParam(m, values, keyOf(buf, p, "feedback"), d.feedbackDb);
        t.axisAzimuthDeg = readParam(m, values, keyOf(buf, p, "az"), d.axisAzimuthDeg);
        t.axisElevationDeg = readParam(m, values, keyOf(buf, p, "el"), d.axisElevationDeg);
        t.spinDeg = readParam(m, values, keyOf(buf, p, "spin"), d.spinDeg);
        t.skew = readParam(m, values, keyOf(buf, p, "skew"), d.skew);
        t.blurDeg = readParam(m, values, keyOf(buf, p, "blur"), d.blurDeg);
        t.lowCutHz = readParam(m, values, keyOf(buf, p, "low_cut"), d.lowCutHz);
        t.highCutHz = readParam(m, values, keyOf(buf, p, "high_cut"), d.highCutHz);
    }
    s.sendShape = sanitised(sendShape);
    s.send = readRegionSettings(m, values, "region1");
    s.sendSide = readRegionSide(m, values, "region1");
    s.sendAmount = readParam(m, values, "mod.amount.region1", s.sendAmount);
    s.sendRates = readRegionRates(m, values, "region1");
    s.ratesContinue = readChoice(m, values, "rates.retrigger", s.ratesContinue ? 1 : 0) > 0;
    s.wetDb = readParam(m, values, "output.wet", s.wetDb);
    s.dryDb = readParam(m, values, "output.dry", s.dryDb);
    return s;
}

EchoFrame EchoResolver::resolve(const EchoSettings& s, double bpm, double sampleRate, int order,
                                bool playing) noexcept {
    EchoFrame frame;
    for (int k = 0; k < kEchoTaps; ++k) {
        const EchoTapSettings& in = s.taps[static_cast<std::size_t>(k)];
        EchoTapFrame& out = frame.taps[static_cast<std::size_t>(k)];
        out.on = in.on;

        const TapTimes times = tapTimes(in.timing, bpm, order);
        const double exact = times.periodSeconds * sampleRate;
        int& hold = held_[static_cast<std::size_t>(k)];
        //  Rounded just below the half: a sixteenth at 120 bpm and 44.1 kHz is exactly 5512.5 samples,
        //  and plain rounding would pick 5512 or 5513 by the tempo's last bit.
        const auto rounded = static_cast<int>(std::floor(exact + 0.5 - 1e-3));
        //  Release the held period only when the time has moved by more than rounding could explain.
        if (hold <= 0 || (rounded != hold && std::abs(exact - hold) > 0.5 + 1e-3)) hold = std::max(1, rounded);
        out.periodSamples = hold;
        out.offsetSamples = static_cast<int>(std::lround(times.offsetSeconds * sampleRate));

        out.level = gainOf(held(in.levelDb, kLevel));
        out.feedback = gainOf(held(in.feedbackDb, kFeedback));

        const double az = held(in.axisAzimuthDeg, kAzimuth), el = held(in.axisElevationDeg, kElevation);
        out.pass.axis = fromAzEl(az * kDeg2Rad, el * kDeg2Rad);
        out.pass.axisIsPole = el == 90.0;                            // as set, not as computed
        out.pass.spinRad = held(in.spinDeg, kSpin, 0.0) * kDeg2Rad;  // a spin that is not a number is none
        out.pass.skew = held(in.skew, kSkew);
        out.pass.blurRad = held(in.blurDeg, kBlur) * kDeg2Rad;
        out.pass.lowCutHz = held(in.lowCutHz, kLowCut);
        out.pass.highCutHz = held(in.highCutHz, kHighCut);
    }
    //  Advance the rates first, so this step's region includes this step's turn.
    sendTurn_.advance(s.sendRates.yaw, s.sendRates.pitch, s.sendRates.roll,
                      playing || s.ratesContinue ? static_cast<double>(kEchoHop) / sampleRate : 0.0);
    frame.send =
        resolveRegion(s.sendShape, s.sendSide, s.send, sendTurn_.yawRad(), sendTurn_.pitchRad(), sendTurn_.rollRad());
    frame.wetGain = levelOf(s.wetDb, kWet);
    frame.dryGain = levelOf(s.dryDb, kDry);
    frame.sendAmount = held(s.sendAmount, kSendAmount);
    return frame;
}

EchoFrame EchoResolver::step(ModulationEngine& mod, const ParamManifest& m, const std::array<float, kMaxParams>& base,
                             const EchoControlInput& in, double sampleRate) noexcept {
    mod.setParameters(base);
    mod.process(in.self, in.sidechain, in.transport, in.dt);

    //  destination() returns every parameter, modulated or not.
    for (int i = 0; i < m.size(); ++i)
        modulated_[static_cast<std::size_t>(i)] = static_cast<float>(mod.destination(static_cast<ParamId>(i)));

    zeroTurns(sendTurn_, in.zeroRegionTurns);
    return resolve(echoSettingsFrom(m, modulated_, in.sendShape), in.transport.bpm, sampleRate, in.order,
                   in.transport.playing);
}

}  // namespace bambi
