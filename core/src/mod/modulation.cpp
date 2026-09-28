// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/mod/modulation.hpp"

#include <algorithm>
#include <cmath>
#include <type_traits>

#include "bambi/math/vec3.hpp"
#include "bambi/mod/manifest.hpp"

namespace bambi {
namespace {

/// One-pole coefficient. `tauSeconds` is in seconds, named for its unit on purpose: a millisecond
/// argument under the same name once made every destination slew 1000x too long.
double smoothCoef(double dt, double tauSeconds) { return 1.0 - std::exp(-dt / std::max(tauSeconds, 1e-5)); }

double wrap01(double x) {
    x -= std::floor(x);
    return (x < 0.0 || x >= 1.0) ? 0.0 : x;
}

}  // namespace

double lfoShapeValue(LfoShape shape, double phaseTurns) {
    switch (shape) {
        case LfoShape::Sine: return std::sin(2.0 * kPi * phaseTurns);
        case LfoShape::Triangle: return 4.0 * std::abs(phaseTurns - 0.5) - 1.0;
        case LfoShape::Saw: return 2.0 * phaseTurns - 1.0;
        case LfoShape::SampleHold: return 0.0;
    }
    return 0.0;
}

// ---------------------------------------------------------------------------------- Lfo

void Lfo::prepare(double sampleRate) {
    sampleRate_ = sampleRate;
    reset();
}

void Lfo::reset() {
    phase_ = 0.0;
    value_ = 0.0;
    rng_ = seed_;
    primed_ = false;
    held_ = 0.0;
}

double Lfo::nextRandom() {
    //  xorshift32: seeded and identical on every platform, so an offline bounce reproduces the take.
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return static_cast<double>(rng_) / 2147483648.0 - 1.0;  // [-1, 1)
}

double Lfo::process(double dt, const Transport& tp, bool synced, bool continuous, double rateHz, double beatsPerCycle,
                    LfoShape shape, double phaseOffsetTurns) {
    const double prev = phase_;

    if (synced && beatsPerCycle > 1e-9 && !continuous) {
        //  Phase is a pure function of transport position, so a bounce starting mid-timeline
        //  lands on exactly the phase the realtime pass had there.
        phase_ = wrap01(tp.ppq / beatsPerCycle);
    } else if (synced && beatsPerCycle > 1e-9) {
        //  Continuing: the tempo's rate, not its position, so a loop's jump back is run through.
        phase_ = wrap01(phase_ + tp.bpm / 60.0 / beatsPerCycle * dt);
    } else if (continuous || tp.playing) {
        phase_ = wrap01(phase_ + rateHz * dt);
    }
    //  Free and set to restart, it is linked to playback: stopped, it holds, and the transport
    //  starts it from zero on play.

    //  Detecting the wrap rather than counting time keeps sample-and-hold correct when the
    //  phase jumps, which is what a synced LFO does when the host locates.
    if (shape == LfoShape::SampleHold) {
        if (!primed_ || phase_ < prev) {
            held_ = nextRandom();
            primed_ = true;
        }
    }

    value_ = shape == LfoShape::SampleHold ? held_ : lfoShapeValue(shape, wrap01(phase_ + phaseOffsetTurns));
    return value_;
}

// ----------------------------------------------------------------------------- Envelope

void Envelope::reset() {
    stage_ = Stage::Idle;
    value_ = 0.0;
    u_ = 0.0;
    from_ = 0.0;
    lastGate_ = false;
    oneShot_ = false;
}

void Envelope::strike(bool oneShot) {
    stage_ = Stage::Attack;
    //  From where it is, not from zero: a hit while it still sounds rises from there.
    u_ = 0.0;
    from_ = value_;
    oneShot_ = oneShot;
}

double ease(double u, double curve) {
    u = clampd(u, 0.0, 1.0);
    const double k = curve * kCurveExponent;
    if (std::abs(k) < 1e-4) return u;  // 0/0 at exactly zero
    return (1.0 - std::exp(-k * u)) / (1.0 - std::exp(-k));
}

double Envelope::process(double dt, bool gate, const Params& p) {
    const auto enter = [this](Stage next) {
        stage_ = next;
        u_ = 0.0;
        from_ = value_;
    };
    if (gate && !lastGate_) {
        enter(Stage::Attack);
        oneShot_ = false;
    }
    if (!gate && lastGate_) enter(Stage::Release);
    lastGate_ = gate;

    //  `seconds` is the stage's own duration: for attack and release, the labelled time scaled
    //  by how far there is to go, so a release from sustain 0.5 takes half of `releaseS` and a
    //  re-hit part way up finishes its attack sooner. Decay always takes exactly `decayS`.
    //  Scaling progress this way keeps every timing correct at curve 0 regardless of how the
    //  curve bows the stage.
    const auto advance = [this](double dt_, double seconds) {
        u_ += dt_ / std::max(seconds, 1e-5);
        return u_ >= 1.0;
    };
    switch (stage_) {
        case Stage::Idle: value_ = 0.0; break;
        case Stage::Attack: {
            const bool done = advance(dt, p.attackS * std::max(1.0 - from_, 0.0));
            value_ = from_ + (1.0 - from_) * ease(u_, p.attackCurve);
            if (done) {
                value_ = 1.0;
                enter(Stage::Decay);
            }
            break;
        }
        case Stage::Decay: {
            const double target = clampd(p.sustain, 0.0, 1.0);
            const bool done = advance(dt, p.decayS);
            value_ = from_ + (target - from_) * ease(u_, p.decayCurve);
            //  A one-shot does not wait at sustain for a gate that will never close.
            if (done) {
                value_ = target;
                enter(oneShot_ ? Stage::Release : Stage::Sustain);
            }
            break;
        }
        case Stage::Sustain: value_ = clampd(p.sustain, 0.0, 1.0); break;
        case Stage::Release: {
            const bool done = advance(dt, p.releaseS * std::max(from_, 0.0));
            value_ = from_ * (1.0 - ease(u_, p.releaseCurve));
            if (done) {
                value_ = 0.0;
                enter(Stage::Idle);
            }
            break;
        }
    }
    return value_;
}

// ------------------------------------------------------------------------- destinations

//  destinationKind and destinationSmoothing are a plugin's own answer and live beside its list.

// ---------------------------------------------------------------------- ModulationEngine

void ModulationEngine::prepare(double sampleRate) {
    sampleRate_ = sampleRate;
    for (auto& l : lfos_) l.prepare(sampleRate);
    reset();
}

ModulationEngine::ModulationEngine() {
    //  Parameters start at zero, not at a plugin's defaults: this engine does not know whose it
    //  is until useManifest. Every owner sets them before the first step.
    //  Seeded here too, so an engine that only ever adopts patches does not run every LFO from
    //  the same seed, with three S&H generators moving in lockstep.
    seedLfos();
}

void ModulationEngine::seedLfos() {
    for (int i = 0; i < kNumLfos; ++i)
        lfos_[static_cast<std::size_t>(i)].setSeed(0x9E3779B9u + static_cast<std::uint32_t>(i) * 0x85EBCA6Bu);
}

static_assert(std::is_same_v<decltype(PluginState::envTriggers), decltype(ModulationPatch::envTriggers)>,
              "a patch carries exactly the state's envelope triggers");

ModulationPatch ModulationPatch::compile(const ModManifest& m, const PluginState& st) {
    ModulationPatch patch;
    for (const auto& c : st.matrix) {
        //  Drop cells that cannot do anything rather than testing them every block: an
        //  unroutable target or a zero depth is a cell the user has emptied, and the matrix
        //  is mostly empty in practice.
        if (m.params->destKindOf(static_cast<int>(c.target)) == DestKind::NotModulatable) continue;
        if (c.depth == 0.0) continue;
        const int slot = sourceSlot(c.tab, c.source);
        if (slot < 0 || slot >= kNumSources) continue;
        patch.cells.push_back(c);
    }
    patch.envTriggers = st.envTriggers;
    return patch;
}

void ModulationEngine::setState(const PluginState& st) {
    owned_ = ModulationPatch::compile(manifest(), st);
    patch_ = &owned_;
    params_ = st.params;
    seedLfos();  // seeds are fixed, so this restarts the LFOs -- as setState always has
}

void ModulationEngine::reset() {
    for (auto& l : lfos_) l.reset();
    restartTransport();  // everything else; the LFOs set to restart are reset twice, to the same state
}

void ModulationEngine::restartTransport() {
    const ModManifest& M = manifest();
    for (int i = 0; i < kNumLfos; ++i)
        if (params_[static_cast<std::size_t>(M.lfos[static_cast<std::size_t>(i)].retrigger)] < 0.5f)
            lfos_[static_cast<std::size_t>(i)].reset();
    for (auto& e : envs_) e.reset();
    sources_.fill(0.0);
    dest_.fill(0.0);
    railing_.fill(false);
    first_ = true;
    for (auto& g : midi_) g = MidiGate{};
}

void ModulationEngine::note(const NoteEvent& e) noexcept {
    if (e.note < 0 || e.note > 127) return;
    const bool on = e.on && e.velocity > 0.0f;
    for (int i = 0; i < kNumEnvelopes; ++i) {
        const auto k = static_cast<std::size_t>(i);
        const auto& t = patch_->envTriggers[k];
        if (t.input != TriggerInput::Midi) continue;
        if (e.note < t.noteLow || e.note > t.noteHigh) continue;
        if (t.channel != 0 && e.channel != t.channel) continue;
        MidiGate& g = midi_[k];
        if (on) {
            g.held.set(static_cast<std::size_t>(e.note));
            g.struck = true;
            g.velocity = std::clamp(static_cast<double>(e.velocity), 0.0, 1.0);
        } else {
            g.held.reset(static_cast<std::size_t>(e.note));
        }
    }
}

void ModulationEngine::allNotesOff() noexcept {
    for (auto& g : midi_) g.held.reset();
}

void ModulationEngine::process(std::span<const double> selfFeatures, std::span<const double> sidechainFeatures,
                               std::span<const double> regionValues, const Transport& tp, double dt) {
    const auto& P = params_;
    const ModManifest& M = manifest();
    const auto param = [&](int at) { return static_cast<double>(P[static_cast<std::size_t>(at)]); };

    // ---- 1. collect raw source values -------------------------------------------------
    std::array<double, kNumSources> raw{};
    for (int i = 0; i < kSourcesPerTab; ++i) {
        const auto k = static_cast<std::size_t>(i);
        raw[k] = k < selfFeatures.size() ? selfFeatures[k] : 0.0;
        //  An absent sidechain reads zero, not stale. A source that holds its last value
        //  when its input goes away keeps modulating something that is no longer there.
        raw[k + kSourcesPerTab] = k < sidechainFeatures.size() ? sidechainFeatures[k] : 0.0;
    }

    for (int i = 0; i < kNumLfos; ++i) {
        //  Every field by name (mod/manifest.hpp): the key list is append-only, so an LFO's
        //  settings cannot be reached by a fixed stride.
        const LfoParams& L = M.lfos[static_cast<std::size_t>(i)];
        const double rate = static_cast<double>(P[static_cast<std::size_t>(L.rate)]);
        const bool sync = P[static_cast<std::size_t>(L.sync)] > 0.5;
        const int div = static_cast<int>(std::lround(P[static_cast<std::size_t>(L.div)]));
        const auto shape = static_cast<LfoShape>(
            std::clamp(static_cast<int>(std::lround(P[static_cast<std::size_t>(L.shape)])), 0, 3));
        const double phase = static_cast<double>(P[static_cast<std::size_t>(L.phase)]) / 360.0;

        const bool continuous = P[static_cast<std::size_t>(L.retrigger)] > 0.5;

        //  lfo*.div choice order, in quarter notes: 8/1 down to 1/16, each followed by its triplet,
        //  which is two thirds as long.
        static constexpr double kBeats[] = {32.0, 64.0 / 3.0, 16.0, 32.0 / 3.0, 8.0, 16.0 / 3.0, 4.0,  8.0 / 3.0,
                                            2.0,  4.0 / 3.0,  1.0,  2.0 / 3.0,  0.5, 1.0 / 3.0,  0.25, 1.0 / 6.0};
        const double beats = kBeats[std::clamp(div, 0, 15)];

        const double polarity = std::clamp(static_cast<double>(P[static_cast<std::size_t>(L.polarity)]), 0.0, 1.0);

        //  Converted to [0, 1] before polarise(), the same shape an envelope already uses.
        const double played =
            lfos_[static_cast<std::size_t>(i)].process(dt, tp, sync, continuous, rate, beats, shape, phase);
        raw[static_cast<std::size_t>(12 + i)] = polarise(0.5 * (played + 1.0), polarity);
    }

    for (int i = 0; i < kNumEnvelopes; ++i) {
        const auto k = static_cast<std::size_t>(i);
        const auto& t = patch_->envTriggers[k];
        Envelope& env = envs_[k];

        const EnvParams& E = M.envelopes[k];
        Envelope::Params ep;
        ep.attackS = static_cast<double>(P[static_cast<std::size_t>(E.attack)]) * 0.001;  // ms at the boundary only
        ep.decayS = static_cast<double>(P[static_cast<std::size_t>(E.decay)]) * 0.001;
        ep.sustain = static_cast<double>(P[static_cast<std::size_t>(E.sustain)]);
        ep.releaseS = static_cast<double>(P[static_cast<std::size_t>(E.release)]) * 0.001;
        ep.attackCurve = static_cast<double>(P[static_cast<std::size_t>(E.attackCurve)]);
        ep.decayCurve = static_cast<double>(P[static_cast<std::size_t>(E.decayCurve)]);
        ep.releaseCurve = static_cast<double>(P[static_cast<std::size_t>(E.releaseCurve)]);

        bool gate = false;
        double level = 1.0;
        if (t.input == TriggerInput::Midi) {
            //  A held note released before its own step (a note-on and note-off inside one
            //  control hop) plays as a one-shot rather than leaving the envelope parked at
            //  sustain. Velocity scales the envelope by the trigger's velocity amount; at 0
            //  every hit is alike.
            MidiGate& g = midi_[k];
            gate = t.gate == TriggerGate::Held && g.held.any();
            if (g.struck) env.strike(t.gate == TriggerGate::OneShot || !gate);
            g.struck = false;
            level = 1.0 - t.velocity + t.velocity * g.velocity;
        } else {
            const auto src = static_cast<std::size_t>(std::clamp(t.source, 0, kNumSources - 1));
            //  Hysteresis on the gate: a feature hovering at the threshold would otherwise
            //  retrigger every control step and turn the envelope into a buzz.
            const bool wasOn = env.value() > 0.0;
            const double on = wasOn ? t.threshold - t.hysteresis : t.threshold;
            gate = raw[src] > on;
        }
        const double polarity = clampd(static_cast<double>(P[static_cast<std::size_t>(E.polarity)]), 0.0, 1.0);
        raw[static_cast<std::size_t>(15 + i)] = polarise(env.process(dt, gate, ep) * level, polarity);
    }

    //  A region is a value in 0..1 like a feature. The matrix has one region column regardless
    //  of how many slots a plugin has: a second slot is a send or a return, not a second source.
    for (int i = 0; i < columnsIn(MatrixTab::Region); ++i) {
        const auto k = static_cast<std::size_t>(i);
        const double v = k < regionValues.size() ? regionValues[k] : 0.0;
        raw[static_cast<std::size_t>(sourceSlot(MatrixTab::Region, i))] = std::isfinite(v) ? clampd(v, 0.0, 1.0) : 0.0;
    }

    // ---- 2. apply per-source amounts --------------------------------------------------
    for (int i = 0; i < kNumSources; ++i) {
        const auto amount = static_cast<double>(P[static_cast<std::size_t>(M.amountOf(i))]);
        sources_[static_cast<std::size_t>(i)] = raw[static_cast<std::size_t>(i)] * amount;
    }

    // ---- 3. sum the matrix into each destination ---------------------------------------
    std::array<double, kMaxParams> sum{};
    for (const auto& c : patch_->cells) {
        const int slot = sourceSlot(c.tab, c.source);
        sum[static_cast<std::size_t>(c.target)] += sources_[static_cast<std::size_t>(slot)] * c.depth;
    }

    const double global = param(M.globalAmount);
    for (int i = 0; i < M.params->size(); ++i) {
        const auto id = static_cast<ParamId>(i);
        if (M.params->destKindOf(i) == DestKind::NotModulatable) {
            //  No cell can reach it, but it is still read back through destination(), so it
            //  carries its own parameter's value rather than the zero it was left at.
            railing_[static_cast<std::size_t>(i)] = false;
            applyDestination(id, param(i), dt, first_);
            continue;
        }

        const auto& d = (*M.params)[i];
        //  Modulation spans the destination's own range, so a depth of 1.0 means the same
        //  thing on every destination: full deflection covers the full span.
        const double span = d.max - d.min;
        const double target = param(i) + sum[static_cast<std::size_t>(i)] * global * span;
        if (periodOf(i) > 0.0) {
            //  One turn: past an end it comes back in at the other, and never rails.
            railing_[static_cast<std::size_t>(i)] = false;
            applyDestination(id, target, dt, first_);
            continue;
        }
        const double clamped = clampd(target, d.min, d.max);
        railing_[static_cast<std::size_t>(i)] = std::abs(target - clamped) > 1e-9;
        applyDestination(id, clamped, dt, first_);
    }
    first_ = false;
}

void ModulationEngine::applyDestination(ParamId id, double target, double dt, bool first) {
    auto& v = dest_[static_cast<std::size_t>(id)];
    const int at = static_cast<int>(id);
    //  A parameter that repeats is kept within one period of its minimum and moved the short way
    //  round: just below the top to just above the bottom is a small step across the seam, not a
    //  sweep back through the whole range.
    const double period = periodOf(at);
    const double low = (*manifest().params)[at].min;
    const auto into = [&](double x) {
        return period > 0.0 ? low + (x - low) - period * std::floor((x - low) / period) : x;
    };
    if (first) {
        v = into(target);
        return;
    }
    const double gap = period > 0.0 ? std::remainder(target - v, period) : target - v;

    switch (manifest().params->destKindOf(at)) {
        case DestKind::Rate:
        case DestKind::DirectScalar: v += gap * smoothCoef(dt, manifest().params->smoothingOf(at)); break;
        case DestKind::DirectAngle: {
            //  A hard bound on how fast this may move, which is what a one-pole cannot give.
            const double maxStep = manifest().params->smoothingOf(at) * dt;
            v += clampd(gap, -maxStep, maxStep);
            break;
        }
        case DestKind::NotModulatable: v = target; break;
    }
    v = into(v);
}

double ModulationEngine::periodOf(int at) const noexcept {
    const double set = periodOverride_[static_cast<std::size_t>(at)];
    if (set >= 0.0) return set;
    const auto* m = manifest().params;
    return m->wrapsAt(at) ? (*m)[at].max - (*m)[at].min : 0.0;
}

double ModulationEngine::destination(ParamId id) const { return dest_[static_cast<std::size_t>(id)]; }

bool ModulationEngine::railing(ParamId id) const { return railing_[static_cast<std::size_t>(id)]; }

// -------------------------------------------------------------------------- MotionClock

void RotationClock::advance(double yawDegPerSec, double pitchDegPerSec, double rollDegPerSec, double dt) {
    const auto wrapPi = [](double a) {
        //  Into (-pi, pi]: an angle that has gone round is the same angle, and letting it grow without
        //  bound would lose precision on a long take.
        a = std::fmod(a + kPi, 2.0 * kPi);
        if (a < 0.0) a += 2.0 * kPi;
        return a - kPi;
    };
    yawRad_ = wrapPi(yawRad_ + yawDegPerSec * kDeg2Rad * dt);
    pitchRad_ = wrapPi(pitchRad_ + pitchDegPerSec * kDeg2Rad * dt);
    rollRad_ = wrapPi(rollRad_ + rollDegPerSec * kDeg2Rad * dt);
}

double MotionClock::advance(double speedDegPerSec, double lengthRad, double dt) {
    if (lengthRad > 1e-9) phase_ += (speedDegPerSec * kDeg2Rad / lengthRad) * dt;
    return phase_;
}

double MotionClock::sAt(double displace, MovementMode mode, bool closed) const {
    return Trajectory::phaseToS(phase_ + displace, mode, closed);
}

}  // namespace bambi
