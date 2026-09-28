// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/mod/sources.hpp"

#include <algorithm>
#include <cmath>

#include "bambi/mod/modulation.hpp"
#include "bambi/patch/parameters.hpp"

namespace bambi {
namespace {

constexpr std::array<const char*, 12> kNoteNames{"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

/// One level per cycle, for drawing sample-and-hold. Illustrative only: what plays is a seeded draw.
constexpr std::array<double, 6> kIllustrativeSteps{-0.55, 0.64, -0.82, 0.27, 0.91, -0.36};

double wrap01(double x) {
    x -= std::floor(x);
    return (x < 0.0 || x >= 1.0) ? 0.0 : x;
}

std::string ms(double seconds) { return formatNumber(seconds * 1000.0, 0) + " ms"; }

std::string db(double value) { return formatNumber(value, 0) + " dB"; }

std::string dbRange(double from, double to) { return formatNumber(from, 0) + " to " + db(to); }

std::string hz(double f) {
    if (f < 1000.0) return formatNumber(f, 0) + " hz";
    const double k = f / 1000.0;
    const bool whole = std::abs(k - std::round(k)) < 1e-9;
    return formatNumber(k, whole ? 0 : 1) + " khz";
}

std::string followers(double up, double down) { return ms(up) + " \xc2\xb7 " + ms(down); }

}  // namespace

SourceRef sourceAt(int slot) {
    if (slot < 0 || slot >= kNumSources) return {};
    if (slot < 2 * kSourcesPerTab) return {SourceKind::Feature, slot % kSourcesPerTab, slot >= kSourcesPerTab};
    if (slot < 2 * kSourcesPerTab + kNumLfos) return {SourceKind::Lfo, slot - 2 * kSourcesPerTab, false};
    if (slot < 3 * kSourcesPerTab) return {SourceKind::Envelope, slot - 2 * kSourcesPerTab - kNumLfos, false};
    return {SourceKind::Region, slot - 3 * kSourcesPerTab, false};
}

std::vector<OutlinePoint> lfoOutline(LfoShape shape, double phaseDeg, int cycles, int pointsPerCycle) {
    cycles = std::max(1, cycles);
    std::vector<OutlinePoint> out;
    if (shape == LfoShape::SampleHold) {
        //  The latch is on the LFO's own cycle, before the phase offset (Lfo::process): an offset
        //  moves nothing here.
        out.reserve(static_cast<std::size_t>(2 * cycles));
        for (int c = 0; c < cycles; ++c) {
            const double y = kIllustrativeSteps[static_cast<std::size_t>(c) % kIllustrativeSteps.size()];
            out.push_back({static_cast<double>(c) / cycles, y});
            out.push_back({static_cast<double>(c + 1) / cycles, y});
        }
        return out;
    }
    const int n = cycles * std::max(2, pointsPerCycle);
    const double offset = phaseDeg / 360.0;
    out.reserve(static_cast<std::size_t>(n + 1));
    for (int i = 0; i <= n; ++i) {
        const double x = static_cast<double>(i) / n;
        out.push_back({x, lfoShapeValue(shape, wrap01(x * cycles + offset))});
    }
    return out;
}

std::array<OutlinePoint, 5> envelopeOutline(double attackMs, double decayMs, double sustain, double releaseMs,
                                            TriggerGate gate) {
    const auto width = [](double t) { return std::sqrt(std::max(t, 0.0)); };
    const double a = width(attackMs);
    const double d = width(decayMs);
    const double h = gate == TriggerGate::Held ? width(kHeldSustainMs) : 0.0;
    const double r = width(releaseMs);
    const double total = std::max(a + d + h + r, 1e-12);
    const double s = std::clamp(sustain, 0.0, 1.0);  // as Envelope::process clamps it
    return {{{0.0, 0.0}, {a / total, 1.0}, {(a + d) / total, s}, {(a + d + h) / total, s}, {1.0, 0.0}}};
}

std::vector<OutlinePoint> envelopeCurve(double attackMs, double decayMs, double sustain, double releaseMs,
                                        TriggerGate gate, double attackCurve, double decayCurve, double releaseCurve,
                                        int perStage) {
    /*  The same shape, over the live total: the normalised picture is the sqrt-unit one divided by
        its own end, and a second sampling loop beside `envelopeStageCurve` would be a copy. */
    const EnvelopeShape env{attackMs, decayMs, sustain, releaseMs, attackCurve, decayCurve, releaseCurve};
    auto out = envelopeStageCurve(env, perStage);
    /*  Over the LIVE total, and with the gate's own hold: this one is the static preview a matrix
        column draws, where a one-shot really does go from decay into release. */
    const auto stops = envelopeStops(env);
    const double hold = stops[3] - stops[2];
    const double total = std::max(stops[4] - (gate == TriggerGate::Held ? 0.0 : hold), 1e-12);
    for (auto& p : out) {
        if (gate != TriggerGate::Held && p.x > stops[2]) p.x -= std::min(p.x - stops[2], hold);
        p.x /= total;
    }
    return out;
}

namespace {

double stageWidth(double ms) { return std::sqrt(std::max(ms, 0.01)); }

// A little above the defaults, not near the maxima: near-maximum values would leave the default
// envelope using about half the plot and looking lost in it.
const double kReferenceSpan = stageWidth(60.0) + stageWidth(600.0) + stageWidth(kHeldSustainMs) + stageWidth(800.0);

/// Heckbert's nice number: the 1, 2, 5 or 10 nearest `range`, at its own magnitude.
double niceNumber(double range, bool round) {
    if (range <= 0.0) return 1.0;
    const double exponent = std::floor(std::log10(range));
    const double fraction = range / std::pow(10.0, exponent);
    const double nice = round ? (fraction < 1.5   ? 1.0
                                 : fraction < 3.0 ? 2.0
                                 : fraction < 7.0 ? 5.0
                                                  : 10.0)
                              : (fraction <= 1.0   ? 1.0
                                 : fraction <= 2.0 ? 2.0
                                 : fraction <= 5.0 ? 5.0
                                                   : 10.0);
    return nice * std::pow(10.0, exponent);
}

}  // namespace

std::array<double, 5> envelopeStops(const EnvelopeShape& env) {
    const double a = stageWidth(env.attackMs);
    const double d = stageWidth(env.decayMs);
    const double h = stageWidth(kHeldSustainMs);  // the preview plateau, always
    const double r = stageWidth(env.releaseMs);
    return {{0.0, a, a + d, a + d + h, a + d + h + r}};
}

double envelopeSpan(const EnvelopeShape& env) { return std::max(kReferenceSpan, envelopeStops(env)[4]); }

std::vector<OutlinePoint> envelopeStageCurve(const EnvelopeShape& env, int perStage) {
    const auto stops = envelopeStops(env);
    const double sustain = std::clamp(env.sustain, 0.0, 1.0);  // as Envelope::process clamps it
    const int steps = std::max(perStage, 2);

    std::vector<OutlinePoint> out;
    out.reserve(static_cast<std::size_t>(3 * steps + 2));
    const auto stage = [&](OutlinePoint from, OutlinePoint to, double curve, bool first) {
        for (int i = first ? 0 : 1; i <= steps; ++i) {
            const double u = static_cast<double>(i) / steps;
            out.push_back({from.x + (to.x - from.x) * u, from.y + (to.y - from.y) * ease(u, curve)});
        }
    };
    stage({stops[0], 0.0}, {stops[1], 1.0}, env.attackCurve, true);
    stage({stops[1], 1.0}, {stops[2], sustain}, env.decayCurve, false);
    out.push_back({stops[3], sustain});  // the hold: level, with nothing to bow
    stage({stops[3], sustain}, {stops[4], 0.0}, env.releaseCurve, false);
    return out;
}

OutlinePoint envelopeGrabPoint(const EnvelopeShape& env, EnvelopeGrab grab) {
    const auto stops = envelopeStops(env);
    const double sustain = std::clamp(env.sustain, 0.0, 1.0);
    //  a curve handle sits at its stage's MIDPOINT, which is on the bowed line and not on the chord
    const auto middle = [](double x0, double y0, double x1, double y1, double curve) {
        return OutlinePoint{0.5 * (x0 + x1), y0 + (y1 - y0) * ease(0.5, curve)};
    };
    switch (grab) {
        case EnvelopeGrab::Attack: return {stops[1], 1.0};
        case EnvelopeGrab::DecaySustain: return {stops[2], sustain};
        case EnvelopeGrab::SustainEdge: return {stops[3], sustain};
        case EnvelopeGrab::Release: return {stops[4], 0.0};
        case EnvelopeGrab::AttackCurve: return middle(stops[0], 0.0, stops[1], 1.0, env.attackCurve);
        case EnvelopeGrab::DecayCurve: return middle(stops[1], 1.0, stops[2], sustain, env.decayCurve);
        case EnvelopeGrab::ReleaseCurve: return middle(stops[3], sustain, stops[4], 0.0, env.releaseCurve);
        case EnvelopeGrab::None: break;
    }
    return {};
}

EnvelopeGrab envelopeGrabAt(const EnvelopeShape& env, double x, double y, double xSlop, double ySlop) {
    const auto stops = envelopeStops(env);
    const double sustain = std::clamp(env.sustain, 0.0, 1.0);

    for (const auto grab :
         {EnvelopeGrab::Attack, EnvelopeGrab::DecaySustain, EnvelopeGrab::SustainEdge, EnvelopeGrab::Release}) {
        const auto at = envelopeGrabPoint(env, grab);
        const double dx = (x - at.x) / std::max(xSlop, 1e-9);
        const double dy = (y - at.y) / std::max(ySlop, 1e-9);
        if (dx * dx + dy * dy <= 1.0) return grab;
    }

    /*  A stage's BODY, away from either end: near an end is that end's point, which is why the
        points are tested first and the ends are excluded here rather than ranked afterwards. */
    struct Body {
        EnvelopeGrab grab;
        double x0, y0, x1, y1, curve;
    };
    const std::array<Body, 3> bodies{{
        {EnvelopeGrab::AttackCurve, stops[0], 0.0, stops[1], 1.0, env.attackCurve},
        {EnvelopeGrab::DecayCurve, stops[1], 1.0, stops[2], sustain, env.decayCurve},
        {EnvelopeGrab::ReleaseCurve, stops[3], sustain, stops[4], 0.0, env.releaseCurve},
    }};
    for (const auto& body : bodies) {
        if (x < body.x0 + xSlop || x > body.x1 - xSlop) continue;
        const double u = (x - body.x0) / std::max(body.x1 - body.x0, 1e-12);
        const double v = body.y0 + (body.y1 - body.y0) * ease(u, body.curve);
        if (std::abs(y - v) <= ySlop) return body.grab;
    }
    return EnvelopeGrab::None;
}

double envelopeStageMs(const EnvelopeShape& env, EnvelopeGrab grab, double x, double loMs, double hiMs) {
    const auto stops = envelopeStops(env);
    //  each stage solves for its OWN width, holding the others at what they are: they are not being dragged
    double width = 0.0;
    switch (grab) {
        case EnvelopeGrab::Attack: width = x; break;
        case EnvelopeGrab::DecaySustain: width = x - stops[1]; break;
        case EnvelopeGrab::Release: width = x - stops[3]; break;
        default: return 0.0;
    }
    width = std::clamp(width, stageWidth(loMs), stageWidth(hiMs));
    return width * width;
}

double envelopeBow(double curve, EnvelopeGrab grab, double dy, double perUnit) {
    const bool rising = grab == EnvelopeGrab::AttackCurve;
    if (grab != EnvelopeGrab::AttackCurve && grab != EnvelopeGrab::DecayCurve && grab != EnvelopeGrab::ReleaseCurve)
        return curve;
    const double delta = (rising ? -1.0 : 1.0) * dy / std::max(perUnit, 1e-9);
    return std::clamp(curve + delta, -1.0, 1.0);
}

std::vector<AxisTick> polarityAxis(double polarity, int maxTicks) {
    //  the affine map polarity applies after the curve: floor -polarity, ceiling always 1
    const double offset = -std::clamp(polarity, 0.0, 1.0);
    const double amplitude = 1.0 - offset;
    const double step = niceNumber(niceNumber(amplitude, false) / std::max(1, maxTicks - 1), true);

    std::vector<AxisTick> out;
    if (step <= 0.0 || amplitude <= 1e-9) return out;
    const double first = std::floor(offset / step) * step;
    for (double v = first; v <= 1.0 + step * 0.5; v += step) {
        const double value = std::abs(v) < step * 1e-6 ? 0.0 : v;
        const double at = (value - offset) / amplitude;
        if (at < -0.001 || at > 1.001) continue;  // off the visible curve: not a line on this plot
        out.push_back({value, std::clamp(at, 0.0, 1.0), std::abs(value) < 1e-9});
    }
    return out;
}

std::string axisTickLabel(double value) {
    const double v = std::abs(value) < 1e-9 ? 0.0 : value;
    auto text = formatNumber(v, std::abs(v) >= 1.0 ? 1 : 2);
    //  no trailing zeros: the step's own precision, so "0.5" rather than "0.50"
    if (text.find('.') != std::string::npos) {
        text.erase(text.find_last_not_of('0') + 1);
        if (!text.empty() && text.back() == '.') text.pop_back();
    }
    return text;
}

std::string noteName(int note) {
    note = std::clamp(note, 0, 127);
    const int octave = note / 12 - 2;
    return std::string(kNoteNames[static_cast<std::size_t>(note % 12)]) + formatNumber(static_cast<double>(octave), 0);
}

std::string noteRangeName(int low, int high) {
    const int a = std::min(low, high);
    const int b = std::max(low, high);
    return a == b ? noteName(a) : noteName(a) + " \xe2\x80\x93 " + noteName(b);
}

std::string channelName(int channel) {
    return channel <= 0 ? std::string("any") : std::to_string(std::min(channel, 16));
}

void setTriggerLow(EnvTrigger& t, int note) {
    t.noteLow = std::clamp(note, 0, 127);
    t.noteHigh = std::max(t.noteHigh, t.noteLow);
}

void setTriggerHigh(EnvTrigger& t, int note) {
    t.noteHigh = std::clamp(note, 0, 127);
    t.noteLow = std::min(t.noteLow, t.noteHigh);
}

void setTriggerChannel(EnvTrigger& t, int channel) { t.channel = std::clamp(channel, 0, 16); }

void setTriggerVelocity(EnvTrigger& t, double amount) { t.velocity = std::clamp(amount, 0.0, 1.0); }

void setTriggerThreshold(EnvTrigger& t, double threshold) {
    t.threshold = std::clamp(threshold, 0.0, 1.0);
    t.hysteresis = std::min(t.hysteresis, t.threshold);
}

void setTriggerHysteresis(EnvTrigger& t, double hysteresis) { t.hysteresis = std::clamp(hysteresis, 0.0, t.threshold); }

void setTriggerSource(EnvTrigger& t, int slot) { t.source = std::clamp(slot, 0, kTriggerSources - 1); }

void learnTriggerNote(EnvTrigger& t, int channel, int note) {
    t.input = TriggerInput::Midi;
    t.noteLow = t.noteHigh = std::clamp(note, 0, 127);
    t.channel = std::clamp(channel, 1, 16);
}

std::vector<SourceFact> featureCalibration(Feature f, const FeatureConfig& c) {
    std::vector<SourceFact> out;
    switch (f) {
        case Feature::Level:
            out = {{"range", dbRange(c.levelLoDb, c.levelHiDb)},
                   {"rise", ms(c.levelAttackS)}};  // the fall is a parameter, not a fact
            break;
        case Feature::Attack:
            out = {{"ratio", formatNumber(c.attackLo, 1) + " to " + formatNumber(c.attackHi, 1)},
                   {"fast follower", followers(c.attackFastAttackS, c.attackFastReleaseS)},
                   {"slow follower", followers(c.attackSlowAttackS, c.attackSlowReleaseS)}};
            break;
        case Feature::Tonal:
            out = {{"flatness", dbRange(c.tonalLoDb, c.tonalHiDb)},
                   {"band", hz(c.tonalLoHz) + " to " + hz(c.tonalHiHz)},
                   {"smoothing", ms(c.tonalSmoothS)}};
            break;
        case Feature::Low:
            out = {{"band", "below " + hz(c.bandSplitLoHz)}, {"range", dbRange(c.bandLoDb, c.bandHiDb)}};
            break;
        case Feature::Mid:
            out = {{"band", hz(c.bandSplitLoHz) + " to " + hz(c.bandSplitHiHz)},
                   {"range", dbRange(c.bandLoDb, c.bandHiDb)}};
            break;
        case Feature::High:
            out = {{"band", "above " + hz(c.bandSplitHiHz)}, {"range", dbRange(c.bandLoDb, c.bandHiDb)}};
            break;
    }
    //  every feature but Level reads zero below it; Level's own range ends there instead
    if (f != Feature::Level) out.push_back({"gate", db(c.gateDb)});
    return out;
}

}  // namespace bambi
