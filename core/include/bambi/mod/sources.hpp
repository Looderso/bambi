// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <string>
#include <vector>

#include "bambi/dsp/features.hpp"
#include "bambi/mod/modulation.hpp"
#include "bambi/patch/state.hpp"

namespace bambi {

/// A modulation source as its settings show it: a feature of the input or sidechain, an LFO, or a
/// generator envelope. Pure functions over the engine's own definitions, so what the editor draws
/// matches what the engine does.

enum class SourceKind { None, Feature, Lfo, Envelope, Region };

struct SourceRef {
    SourceKind kind{SourceKind::None};
    int index{0};           ///< feature 0-5 (Feature order), LFO 0-2, envelope 0-2
    bool sidechain{false};  ///< a feature of the sidechain input
};

SourceRef sourceAt(int slot);  ///< 0-5 self, 6-11 sidechain, 12-14 LFOs, 15-17 envelopes; None outside

struct OutlinePoint {  ///< a corner of a drawn outline: x across [0, 1], y in the source's own range
    double x{0.0};
    double y{0.0};
};

/// `cycles` of an LFO with its phase offset, for drawing: y in [-1, 1]. Sample-and-hold draws one
/// step per cycle from a fixed illustrative sequence; the real sequence is a seeded draw.
std::vector<OutlinePoint> lfoOutline(LfoShape shape, double phaseDeg, int cycles, int pointsPerCycle);

/// An ADSR's outline: five corners with y in [0, 1] -- start, peak, end of decay, end of sustain, end
/// of release. Each stage is as wide as the square root of its time, since times from 0.1 ms to 8 s
/// cannot share a linear axis; a held gate adds a sustain plateau `kHeldSustainMs` wide, a one-shot
/// none.
inline constexpr double kHeldSustainMs = 300.0;
std::array<OutlinePoint, 5> envelopeOutline(double attackMs, double decayMs, double sustain, double releaseMs,
                                            TriggerGate gate);

/// The same shape, bowed per timed stage by its own curve, over the live total so the drawing matches what plays.
std::vector<OutlinePoint> envelopeCurve(double attackMs, double decayMs, double sustain, double releaseMs,
                                        TriggerGate gate, double attackCurve, double decayCurve, double releaseCurve,
                                        int perStage);

/// The editor's own geometry for the envelope picture: where every part of it sits, what is under a
/// point, and what a drag makes of it. X is in sqrt-units rather than a fraction of the width, so a
/// stage's x depends only on its own time and a drag of it inverts exactly.
struct EnvelopeShape {
    double attackMs{10.0}, decayMs{200.0}, sustain{0.5}, releaseMs{400.0};
    double attackCurve{0.0}, decayCurve{0.0}, releaseCurve{0.0};
};

std::array<double, 5> envelopeStops(
    const EnvelopeShape& env);  ///< cumulative sqrt-units at start, peak, end of decay, end of hold, end of release

double envelopeSpan(
    const EnvelopeShape&
        env);  ///< sqrt-units the plot scales against: a reference span above the defaults, or the shape's own when larger, so it doesn't rescale under every drag

std::vector<OutlinePoint> envelopeStageCurve(
    const EnvelopeShape& env, int perStage);  ///< x in sqrt-units; `envelopeCurve` is this over `envelopeSpan`

/// What a point of the picture is: four points set times and the sustain level; three curve handles,
/// at each bowed stage's midpoint, set nothing but its curve.
enum class EnvelopeGrab { None, Attack, DecaySustain, SustainEdge, Release, AttackCurve, DecayCurve, ReleaseCurve };

OutlinePoint envelopeGrabPoint(const EnvelopeShape& env,
                               EnvelopeGrab grab);  ///< x in sqrt-units, y in [0, 1]; both zero for `None`

/// What is under a point, x and reach in sqrt-units, y and reach in [0, 1]. Points are tested before
/// stage bodies, so a click near a stage's end is that end's.
EnvelopeGrab envelopeGrabAt(const EnvelopeShape& env, double x, double y, double xSlop, double ySlop);

/// Where a point drag puts its stage, in ms, held between `loMs` and `hiMs`, solved from the target x
/// absolutely on every move rather than accumulated from a delta. Zero for a grab that sets no time.
double envelopeStageMs(const EnvelopeShape& env, EnvelopeGrab grab, double x, double loMs, double hiMs);

/// A bowed curve after a drag of `dy` down the screen, `perUnit` of it to a whole curve. A rising
/// stage bulges toward a higher value as its curve grows; a falling one shrinks instead, so both
/// bend away from the mouse.
double envelopeBow(double curve, EnvelopeGrab grab, double dy, double perUnit);

/// The y axis, which is what polarity moves; an LFO's wave and an envelope share it. A tick's `value`
/// is what it says and `at` is where it lands on the curve's own [0, 1]; `zero` marks the tick that
/// is zero, for heavier drawing.
struct AxisTick {
    double value{0.0};
    double at{0.0};
    bool zero{false};
};
std::vector<AxisTick> polarityAxis(double polarity, int maxTicks);
std::string axisTickLabel(double value);  ///< to the step's own precision: "0.5", not "0.50"

std::string noteName(int note);                ///< "C1" for 36: General MIDI octave numbering, middle C (60) is C3
std::string noteRangeName(int low, int high);  ///< "C1", or "C1 – E1"
std::string channelName(int channel);          ///< "any", or "1" to "16"

inline constexpr int kTriggerSources = 2 * kSourcesPerTab;  ///< a feature of either input: slots 0-11

/// Setters that keep a trigger valid: a note range never inverts, and hysteresis never exceeds the
/// threshold (release point threshold - hysteresis).
void setTriggerLow(EnvTrigger& t, int note);
void setTriggerHigh(EnvTrigger& t, int note);
void setTriggerChannel(EnvTrigger& t, int channel);     ///< 0 (any) to 16
void setTriggerVelocity(EnvTrigger& t, double amount);  ///< 0 to 1
void setTriggerThreshold(EnvTrigger& t, double threshold);
void setTriggerHysteresis(EnvTrigger& t, double hysteresis);
void setTriggerSource(EnvTrigger& t, int slot);

void learnTriggerNote(EnvTrigger& t, int channel, int note);  ///< MIDI input, that note alone, on that channel

/// A feature's calibration, read from the detector's own configuration so text can't drift from what runs.
struct SourceFact {
    std::string label;
    std::string value;
};
std::vector<SourceFact> featureCalibration(Feature f, const FeatureConfig& cfg = {});

}  // namespace bambi
