// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/reverb/control.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "bambi/patch/readparams.hpp"

namespace bambi {
namespace {

/// A factory preset: room and cuts only, so it never reaches into a user's regions, level or quality.
struct Preset {
    double size;
    RoomShape shape;
    double decay, tone, rough, distance, lowCut, highCut;
};

constexpr std::array<Preset, 6> kPresets{{
    {4.0, RoomShape::Room, 0.35, 0.50, 0.80, 1.5, 180.0, 12000.0},    // ambience
    {5.0, RoomShape::Room, 0.50, 0.42, 0.75, 2.0, 140.0, 12000.0},    // room
    {7.0, RoomShape::Tall, 1.20, 0.55, 0.70, 3.0, 130.0, 13000.0},    // chamber
    {20.0, RoomShape::Hall, 1.90, 0.50, 0.60, 6.0, 150.0, 11000.0},   // hall
    {28.0, RoomShape::Hall, 2.80, 0.45, 0.60, 10.0, 140.0, 10000.0},  // large hall
    {38.0, RoomShape::Tall, 6.50, 0.35, 0.50, 14.0, 120.0, 9000.0},   // cathedral
}};

constexpr ParamSpec kSize = reverbParamSpec("room.size");
constexpr ParamSpec kDecay = reverbParamSpec("room.decay");
constexpr ParamSpec kTone = reverbParamSpec("room.tone");
constexpr ParamSpec kRoughness = reverbParamSpec("room.roughness");
constexpr ParamSpec kDistance = reverbParamSpec("room.distance");
constexpr ParamSpec kWet = reverbParamSpec("output.wet");
constexpr ParamSpec kDry = reverbParamSpec("output.dry");
constexpr ParamSpec kPreDelay = reverbParamSpec("output.pre_delay");
constexpr ParamSpec kLowCut = reverbParamSpec("input.low_cut");
constexpr ParamSpec kHighCut = reverbParamSpec("input.high_cut");
constexpr ParamSpec kSendAmount = reverbParamSpec("mod.amount.region1");
constexpr ParamSpec kReturnAmount = reverbParamSpec("mod.amount.region2");

double clampTo(double v, const ParamSpec& p) { return std::clamp(v, p.min, p.max); }

Region regionOf(const ReverbRegionSettings& r, const RotationClock& turn) {
    return resolveRegion(sanitised(r.shape), r.side, r.deg, turn.yawRad(), turn.pitchRad(), turn.rollRad());
}

}  // namespace

int tailLinesFor(ReverbQuality q) noexcept { return q == ReverbQuality::Efficient ? 8 : 16; }

ReverbSettings ReverbSettings::preset(ReverbPreset p) {
    const Preset& k = kPresets[static_cast<std::size_t>(std::clamp(static_cast<int>(p), 0, 5))];
    ReverbSettings s;
    s.sizeMetres = k.size;
    s.shape = k.shape;
    s.decaySeconds = k.decay;
    s.tone = k.tone;
    s.roughness = k.rough;
    s.distanceMetres = k.distance;
    s.lowCutHz = k.lowCut;
    s.highCutHz = k.highCut;
    return s;
}

ReverbSettings reverbSettingsFrom(const ParamManifest& m, std::span<const float> values, RoomShape shape,
                                  const RegionShape& sendShape, const RegionShape& returnShape,
                                  RenderQuality renderQuality) {
    const ReverbSettings d;
    ReverbSettings s;
    s.sizeMetres = readParam(m, values, "room.size", d.sizeMetres);
    s.shape = shape;
    s.decaySeconds = readParam(m, values, "room.decay", d.decaySeconds);
    s.tone = readParam(m, values, "room.tone", d.tone);
    s.roughness = readParam(m, values, "room.roughness", d.roughness);
    s.distanceMetres = readParam(m, values, "room.distance", d.distanceMetres);

    s.wetDb = readParam(m, values, "output.wet", d.wetDb);
    s.dryDb = readParam(m, values, "output.dry", d.dryDb);
    s.preDelayTrimMs = readParam(m, values, "output.pre_delay", d.preDelayTrimMs);
    s.lowCutHz = readParam(m, values, "input.low_cut", d.lowCutHz);
    s.highCutHz = readParam(m, values, "input.high_cut", d.highCutHz);

    s.send.shape = sanitised(sendShape);
    s.send.side = readRegionSide(m, values, "region1");
    s.send.deg = readRegionSettings(m, values, "region1");
    s.send.rates = readRegionRates(m, values, "region1");
    s.send.amount = readParam(m, values, "mod.amount.region1", kSendAmount.def);
    s.returnTo.shape = sanitised(returnShape);
    s.returnTo.side = readRegionSide(m, values, "region2");
    s.returnTo.deg = readRegionSettings(m, values, "region2");
    s.returnTo.rates = readRegionRates(m, values, "region2");
    s.ratesContinue = readChoice(m, values, "rates.retrigger", d.ratesContinue ? 1 : 0) > 0;
    s.returnTo.amount = readParam(m, values, "mod.amount.region2", kReturnAmount.def);

    const int fallbackQuality = static_cast<int>(reverbDefault("quality.playing"));
    s.quality = readChoice(m, values, "quality.playing", fallbackQuality) > 0 ? ReverbQuality::Realistic
                                                                              : ReverbQuality::Efficient;
    s.renderQuality = renderQuality;
    return s;
}

ReverbFrame resolveReverb(const ReverbSettings& s, bool offline, const RotationClock& sendTurn,
                          const RotationClock& returnTurn) noexcept {
    ReverbFrame f;
    f.room.size = clampTo(s.sizeMetres, kSize);
    f.room.shape = s.shape;
    f.room.decay = clampTo(s.decaySeconds, kDecay);
    f.room.tone = clampTo(s.tone, kTone);
    f.room.roughness = clampTo(s.roughness, kRoughness);
    f.distance = clampTo(s.distanceMetres, kDistance);

    // The bottom of the dry range is off, not "very quiet".
    const double dryDb = clampTo(s.dryDb, kDry);
    f.levelDb = clampTo(s.wetDb, kWet);
    f.dryGain = dryDb <= kDry.min ? 0.0 : std::pow(10.0, dryDb / 20.0);
    f.preDelayTrimSeconds = clampTo(s.preDelayTrimMs, kPreDelay) * 0.001;
    f.lowCutHz = clampTo(s.lowCutHz, kLowCut);
    f.highCutHz = clampTo(s.highCutHz, kHighCut);

    f.send = regionOf(s.send, sendTurn);
    f.sendAmount = clampTo(s.send.amount, kSendAmount);
    f.returnRegion = regionOf(s.returnTo, returnTurn);
    f.returnAmount = clampTo(s.returnTo.amount, kReturnAmount);

    // A render can be asked to take the better quality regardless of the switch, for an offline bounce.
    const bool better = offline && s.renderQuality == RenderQuality::Realistic;
    f.tailLines = tailLinesFor(better ? ReverbQuality::Realistic : s.quality);
    f.tailOrder = 3;
    f.drift = true;
    return f;
}

ReverbFrame ReverbResolver::resolve(const ReverbSettings& s, bool offline, double sampleRate, bool playing) noexcept {
    // The turns advance before this step reads them. Tied to playback unless set to continue regardless.
    const double dt = playing || s.ratesContinue ? static_cast<double>(kReverbHop) / std::max(1.0, sampleRate) : 0.0;
    sendTurn_.advance(s.send.rates.yaw, s.send.rates.pitch, s.send.rates.roll, dt);
    returnTurn_.advance(s.returnTo.rates.yaw, s.returnTo.rates.pitch, s.returnTo.rates.roll, dt);
    return resolveReverb(s, offline, sendTurn_, returnTurn_);
}

ReverbFrame ReverbResolver::step(ModulationEngine& mod, const ParamManifest& m,
                                 const std::array<float, kMaxParams>& base, const ReverbControlInput& in,
                                 double sampleRate) noexcept {
    mod.setParameters(base);
    mod.process(in.self, in.sidechain, in.transport, in.dt);

    // Every parameter as the matrix leaves it: `destination()` passes through a value no cell reaches.
    for (int i = 0; i < m.size(); ++i)
        modulated_[static_cast<std::size_t>(i)] = static_cast<float>(mod.destination(static_cast<ParamId>(i)));

    // An angle set back to zero by hand takes its accumulated turn with it.
    zeroTurns(sendTurn_, in.zeroRegionTurns[0]);
    zeroTurns(returnTurn_, in.zeroRegionTurns[1]);
    return resolve(reverbSettingsFrom(m, modulated_, in.roomShape, in.sendShape, in.returnShape, in.renderQuality),
                   in.offline, sampleRate, in.transport.playing);
}

void applyPreset(const ParamManifest& m, ReverbPreset p, const std::function<void(int, float)>& write,
                 RoomState& room) {
    const ReverbSettings want = ReverbSettings::preset(p);
    // The shape is state, not a parameter: it goes with the selection.
    room.preset = std::clamp(static_cast<int>(p), 0, 5);
    room.shape = static_cast<int>(want.shape);
    const auto set = [&](const char* key, double value) {
        const int at = m.byKey(key);
        if (at == kNoParam || !write) return;
        write(at, toNormalised(m, at, static_cast<float>(value)));
    };
    set("room.size", want.sizeMetres);
    set("room.decay", want.decaySeconds);
    set("room.tone", want.tone);
    set("room.roughness", want.roughness);
    set("room.distance", want.distanceMetres);
    set("input.low_cut", want.lowCutHz);
    set("input.high_cut", want.highCutHz);
}

}  // namespace bambi
