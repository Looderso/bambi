// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <bitset>
#include <cassert>
#include <cstdint>
#include <span>
#include <vector>

#include "bambi/mod/manifest.hpp"
#include "bambi/patch/parameters.hpp"
#include "bambi/patch/state.hpp"
#include "bambi/path/trajectory.hpp"

namespace bambi {

/*  The modulation engine: 18 sources, a matrix of cells, and the destinations they drive.
 *
 *      contribution = source value x source amount x cell depth
 *
 *  Automation rides the per-source amount, never the matrix cell.
 *
 *  Source order is fixed (sourceAmount() matches a slot to its mod.amount.* parameter):
 *
 *      0-5    self features      level attack tonal low mid high
 *      6-11   sidechain/link     the same six, from another source
 *      12-14  LFO 1..3
 *      15-17  Envelope 1..3
 *      18     the region, read where the source was a step ago
 */

/// How far below zero a source reaches. `v` is the shape in [0, 1]; polarity 0 is unipolar, 1 opens it to [-1, 1] bipolar, never past it.
inline constexpr double polarise(double v, double polarity) { return v * (1.0 + polarity) - polarity; }

/// `ease(1, k) == 1` for every k, so a curved stage still reaches its target at exactly its
/// labelled time. `curve` is -1..1, driving exponent `curve * kCurveExponent`; near zero is linear (guarded: 0/0 there).
inline constexpr double kCurveExponent = 7.0;
double ease(double u, double curve);

inline constexpr int kNumLfos = 3;
inline constexpr int kNumEnvelopes = 3;
inline constexpr int kSourcesPerTab = 6;

/// Number of source columns a tab has; the region's tab has one.
constexpr int columnsIn(MatrixTab tab) { return tab == MatrixTab::Region ? 1 : kSourcesPerTab; }

/// Global slot index for a matrix cell's (tab, column), or -1 if the tab has no such column.
constexpr int sourceSlot(MatrixTab tab, int column) {
    if (column < 0 || column >= columnsIn(tab)) return -1;
    return static_cast<int>(tab) * kSourcesPerTab + column;
}

enum class LfoShape { Sine, Triangle, Saw, SampleHold };  ///< matches lfo*.shape choice order

/// Value of `shape` at `phaseTurns` in [0, 1). Sample-and-hold has no value of its own; reads 0.
double lfoShapeValue(LfoShape shape, double phaseTurns);

/// Host transport; `ppq` is quarter notes since the timeline origin, so a synced LFO is a pure function of position.
struct Transport {
    bool playing{false};
    double bpm{120.0};
    double ppq{0.0};
};

/*  An LFO, bipolar [-1, 1]. Deterministic by construction: synced, phase is a function of
 *  transport position; free, it resets on transport start. CONTINUE retrigger runs it all the
 *  time and is never reset, so a bounce no longer repeats what was heard. Sample-and-hold draws
 *  from a generator with a fixed per-LFO seed, not stored in state, for a repeatable bounce.
 */
class Lfo {
public:
    void prepare(double sampleRate);
    void reset();  ///< transport start / offline render start
    void setSeed(std::uint32_t seed) {
        seed_ = seed;
        reset();
    }

    /// One control step; `rateHz` free-running, `beatsPerCycle` synced or continuous.
    double process(double dt, const Transport& tp, bool synced, bool continuous, double rateHz, double beatsPerCycle,
                   LfoShape shape, double phaseOffsetTurns);

    double value() const { return value_; }
    double phase() const { return phase_; }

private:
    double nextRandom();

    double phase_{0.0};  ///< turns, [0,1)
    double value_{0.0};
    double held_{0.0};  ///< current sample-and-hold output
    bool primed_{false};
    std::uint32_t seed_{0x9E3779B9u}, rng_{0x9E3779B9u};
    double sampleRate_{48000.0};
};

/// A triggered ADSR envelope, unipolar [0, 1]: its rest state is genuinely zero; sign comes from the cell depth.
class Envelope {
public:
    struct Params {
        double attackS{0.010}, decayS{0.200}, sustain{0.5}, releaseS{0.400};
        /// Per timed stage, -1..1. Sustain is a level, not a time, and has none.
        double attackCurve{0.0}, decayCurve{0.0}, releaseCurve{0.0};
    };

    void reset();
    double process(double dt, bool gate,
                   const Params& p);  ///< `gate` is the held trigger state; edges are detected here
    double value() const { return value_; }

    /// Restarts the attack from wherever the envelope is. `oneShot` skips waiting at sustain for a gate.
    void strike(bool oneShot);

private:
    enum class Stage { Idle, Attack, Decay, Sustain, Release };
    Stage stage_{Stage::Idle};
    double value_{0.0};
    /// Progress through the current stage and where it started; a curved stage reads its value off the curve instead of integrating it.
    double u_{0.0}, from_{0.0};
    bool oneShot_{false};
    bool lastGate_{false};
};

/// A rate destination is integrated by the motion clock and must not be smoothed here, or
/// integration doubles up. A direct destination is applied instantly, so it is smoothed here;
/// a direct angle is rate-limited instead, since a one-pole bounds no angular velocity.

/// Smoothing constant for a destination: seconds for scalars, degrees/second for angles.

/// A MIDI note as a generator envelope's trigger sees it.
struct NoteEvent {
    int channel{1};        ///< 1..16
    int note{60};          ///< 0..127
    float velocity{1.0f};  ///< 0..1; a note-on at velocity 0 is a note-off, as MIDI defines it
    bool on{true};
};

/// What a patch compiles to: the cells that can do something, and the envelope triggers.
/// Compiling allocates and happens on the message thread; the audio thread adopts a compiled patch by pointer (usePatch), without a lock or allocation.
struct ModulationPatch {
    std::vector<MatrixCell> cells;
    std::array<EnvTrigger, kNumEnvelopes> envTriggers{};

    /// `m` says which plugin's state this is: a cell's target is a position in its manifest.
    static ModulationPatch compile(const ModManifest& m, const PluginState& st);
};

/// The matrix and everything downstream: owns the generators, evaluates every cell, sums per
/// destination, applies the global amount, clamps to range, and smooths. Does not own the motion clock, which is not a modulation source.
class ModulationEngine {
public:
    ModulationEngine();
    //  Not copyable: patch_ may point at owned_, and a copy would point at the original's.
    ModulationEngine(const ModulationEngine&) = delete;
    ModulationEngine& operator=(const ModulationEngine&) = delete;

    void prepare(double sampleRate);

    /// Compiles `st` into a patch this engine owns, takes its parameters, and restarts the LFOs.
    /// Allocates, so it is for offline rendering and tests; the plugin compiles elsewhere and adopts.
    void setState(const PluginState& st);

    /// Adopts a patch compiled elsewhere, by pointer; real-time safe. The caller keeps it alive
    /// until the engine adopts another. Generators carry on; routing does not restart an LFO.
    void usePatch(const ModulationPatch& patch) noexcept { patch_ = &patch; }

    /// A MIDI note, taking effect at the next process(). Real-time safe.
    void note(const NoteEvent& e) noexcept;

    void allNotesOff() noexcept;  ///< all-notes-off, all-sound-off

    /// Updates parameter values only; real-time safe (one fixed-size array copy, no allocation).
    /// setState() also copies the matrix and trajectory, so it cannot run on the audio thread; the plugin calls this instead when a knob moves.
    void setParameters(const std::array<float, kMaxParams>& params) noexcept { params_ = params; }

    /// Which plugin's parameters these are; every field and amount is looked up through it. Set before prepare(); must outlive the engine.
    void useManifest(const ModManifest& m) noexcept { manifest_ = &m; }
    const ModManifest& manifest() const noexcept {
        assert(manifest_ != nullptr && "ModulationEngine::useManifest was never called");
        return *manifest_;
    }

    /// Preparing, or the start of an offline render: everything restarts, even sources set to continue.
    void reset();

    /// Transport start, or a locate: as reset(), except that an LFO set to continue carries on.
    void restartTransport();

    /// Every destination jumps to its value instead of gliding there, since a state or room was chosen whole. Nothing else restarts.
    void settle() { first_ = true; }

    /// One control-rate step. `selfFeatures`/`sidechainFeatures` are 6 values each in Feature enum order; an empty span reads as zero (a disconnected sidechain).
    void process(std::span<const double> selfFeatures, std::span<const double> sidechainFeatures, const Transport& tp,
                 double dt) {
        process(selfFeatures, sidechainFeatures, {}, tp, dt);
    }

    /// As above, plus each region's value at the position the caller supplies from the prior step. Absent, a region source reads zero.
    void process(std::span<const double> selfFeatures, std::span<const double> sidechainFeatures,
                 std::span<const double> regionValues, const Transport& tp, double dt);

    double destination(ParamId id) const;  ///< base parameter + modulation, clamped and smoothed
    double sourceValue(int slot) const {
        return sources_[static_cast<std::size_t>(slot)];
    }  ///< post-amount, for the UI's per-source meters

    bool railing(ParamId id) const;  ///< true when a destination's summed modulation was clipped this step

    /// The period a destination repeats over: unset, a wrapping parameter repeats over its range
    /// and any other is clamped; set, it is never clamped, smoothed the short way round, and kept within one period of its minimum. 0 clamps; below 0 is the default.
    void setPeriod(ParamId id, double period) noexcept { periodOverride_[static_cast<std::size_t>(id)] = period; }

private:
    void applyDestination(ParamId id, double target, double dt, bool first);
    void seedLfos();

    ModulationPatch owned_;
    const ModulationPatch* patch_{&owned_};

    struct MidiGate {
        std::bitset<128> held;  ///< matching notes down now; a set, so a doubled note-on cannot stick it open
        bool struck{false};     ///< a matching note-on since the last step
        double velocity{1.0};   ///< of the latest one
    };
    std::array<MidiGate, kNumEnvelopes> midi_{};
    //  Sized by the cap every plugin shares: this engine is the same engine in all three.
    const ModManifest* manifest_{nullptr};
    std::array<float, kMaxParams> params_{};
    std::array<Lfo, kNumLfos> lfos_;
    std::array<Envelope, kNumEnvelopes> envs_;
    std::array<double, kNumSources> sources_{};
    std::array<double, kMaxParams> dest_{};
    std::array<bool, kMaxParams> railing_{};
    std::array<double, kMaxParams> periodOverride_ = [] {
        std::array<double, kMaxParams> a{};
        a.fill(-1.0);
        return a;
    }();
    double periodOf(int at) const noexcept;
    double sampleRate_{48000.0};
    bool first_{true};
};

/// The path's own rotation, integrated. All three axes wrap, and pitch is a rotation about the
/// world's left axis here, not an elevation that stops at a pole. Offsets add to yaw/pitch/roll
/// rather than overwrite them, so automation and undo stay honest.
class RotationClock {
public:
    void reset() { yawRad_ = pitchRad_ = rollRad_ = 0.0; }

    /// One control step, in degrees per second.
    void advance(double yawDegPerSec, double pitchDegPerSec, double rollDegPerSec, double dt);

    /// Clears one axis's accumulated turn (0 yaw, 1 pitch, 2 roll) when set back to zero by hand.
    void zero(int axis) {
        if (axis == 0)
            yawRad_ = 0.0;
        else if (axis == 1)
            pitchRad_ = 0.0;
        else if (axis == 2)
            rollRad_ = 0.0;
    }

    double yawRad() const { return yawRad_; }
    double pitchRad() const { return pitchRad_; }
    double rollRad() const { return rollRad_; }

private:
    double yawRad_{0.0}, pitchRad_{0.0}, rollRad_{0.0};
};

/// Clears whichever axes a mask names (bit 0 yaw, 1 pitch, 2 roll).
inline constexpr int kTurnAxisBits = 3;
inline void zeroTurns(RotationClock& clock, int mask) {
    for (int axis = 0; axis < kTurnAxisBits; ++axis)
        if ((mask & (1 << axis)) != 0) clock.zero(axis);
}

/// The position clock: a monotonic phase integrator, deliberately not a modulation source
/// (routing it through the matrix's smoother once flung it backwards on a wrap). Speed is
/// degrees per second along the path: divided by the trajectory's arc length, so 90 deg/s covers 90 degrees of travel regardless of path length.
class MotionClock {
public:
    void reset(double phase = 0.0) { phase_ = phase; }

    /// Advance by `speedDegPerSec` along a path of `lengthRad`, and return the new phase.
    double advance(double speedDegPerSec, double lengthRad, double dt);

    /// Displace shifts phase before the movement-mode mapping; applied to the arc length instead, it would park the source at an extreme for most of a lap.
    double sAt(double displace, MovementMode mode, bool closed) const;

    double phase() const { return phase_; }

private:
    double phase_{0.0};
};

}  // namespace bambi
