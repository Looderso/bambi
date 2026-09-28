// SPDX-License-Identifier: GPL-3.0-or-later
//
//  bambi-render — offline ambisonic renderer.
//
//  This is the dome strategy, not a convenience. Access to an array is intermittent, so a
//  session has to be an evaluation rather than a debugging session: render fixtures here,
//  carry the files, decode them there. Nothing has to build or survive in the room.
//
//  It is also the basis of golden-file regression — render, hash, compare — which catches
//  offline-determinism breaks without a DAW.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bambi/dsp/features.hpp"
#include "bambi/encode/control.hpp"
#include "bambi/encode/encoder.hpp"
#include "bambi/encode/params.hpp"
#include "bambi/encode/pathparams.hpp"
#include "bambi/io/wav.hpp"
#include "bambi/math/denormals.hpp"
#include "bambi/mod/beatclock.hpp"
#include "bambi/mod/modulation.hpp"
#include "bambi/patch/state.hpp"
#include "bambi/path/generator.hpp"
#include "bambi/path/trajectory.hpp"

using namespace bambi;

namespace {

constexpr int kBlock = 128;

/*  The control period, in samples. Features already analyse on this grid, so the modulation
 *  engine steps on it too.
 *
 *  It is deliberately not "once per audio block". A host is free to use one buffer size for
 *  realtime and another for an offline bounce, and stepping modulation per block would make
 *  every modulated value a function of that buffer size -- but an offline bounce must be
 *  sample-identical to realtime. Pinning control to a fixed sample grid makes block size
 *  irrelevant by construction rather than by luck, and `--block` exists so the tests can
 *  prove it.
 */

void usage() {
    std::printf(R"(bambi-render — offline ambisonic renderer

  bambi-render <in.wav> <out.wav> [options]
  bambi-render --noise <seconds> <out.wav> [options]
  bambi-render --tone  <seconds> <out.wav> [options]     a chord: Tonal reads high

options
  --preset FILE        load plugin state (JSON) as the starting point
  --order N            ambisonic order, 0..max          (default 3)
  --generator NAME     orbit | lissajous | wave | arc | spiral
  --speed DEG_PER_SEC  angular velocity along the path  (default 90)
  --width DEG          source width, 0..180             (default 0)
  --gain DB            output gain                      (default 0)
  --mode NAME          wrap | pingpong | once           (open paths only)
  --set KEY=VALUE      a generator parameter by name, repeatable
  --mod SRC:DEST=DEPTH a matrix cell, e.g. level:motion.speed=1, repeatable
  --region KIND        give the encoder its region: everywhere | spot | band | sectors | dots.
                       Its settings are parameters (--param region1.size=60); its value is the
                       source region1, and the trace gains a `region` column
  --param KEY=VALUE    any parameter by its key, repeatable
  --bpm N              transport tempo for synced LFOs        (default 120)
  --block N            audio block size; results must not depend on it (default 128)
  --trace FILE         write a CSV of every control step (see below)
  --print              describe the render and exit without writing

sources for --mod: level attack tonal low mid high (self), sc_* (sidechain),
lfo1..3, env1..3.  destinations are parameter keys, e.g. motion.speed, render.width.

--trace writes one row per control step: time, the six features, the resolved
destinations, and the source position in both arc length and az/el. It is the way to
see what a render DID without listening to it, and it is what the golden fixtures
compare -- audio alone can hide a modulation bug behind a plausible-sounding result.

output is 32-bit float WAV, AmbiX: ACN order, SN3D normalisation.
)");
}

bool parseDouble(const char* s, double& out) {
    char* end = nullptr;
    const double v = std::strtod(s, &end);
    if (end == s || *end != '\0') return false;
    out = v;
    return true;
}

GeneratorType generatorByName(const std::string& n, bool& ok) {
    ok = true;
    if (n == "orbit") return GeneratorType::Orbit;
    if (n == "lissajous") return GeneratorType::Lissajous;
    if (n == "wave") return GeneratorType::Wave;
    if (n == "arc") return GeneratorType::Arc;
    if (n == "spiral") return GeneratorType::Spiral;
    ok = false;
    return GeneratorType::Lissajous;
}

MovementMode modeByName(const std::string& n, bool& ok) {
    ok = true;
    if (n == "wrap") return MovementMode::Wrap;
    if (n == "pingpong" || n == "ping-pong") return MovementMode::PingPong;
    if (n == "once") return MovementMode::Once;
    ok = false;
    return MovementMode::PingPong;
}

/*  Source names for --mod, in the fixed slot order the matrix uses. Kept next to nothing
 *  else on purpose: this order is the same one mod.amount.* follows, and a mismatch would
 *  attach the wrong control to the wrong source silently. */
constexpr const char* kSourceNames[] = {
    "level",  "attack",  "tonal", "low",  "mid",  "high", "sc_level", "sc_attack", "sc_tonal", "sc_low",
    "sc_mid", "sc_high", "lfo1",  "lfo2", "lfo3", "env1", "env2",     "env3",      "region1",
};

int sourceByName(const std::string& n) {
    for (int i = 0; i < kNumSources; ++i)
        if (n == kSourceNames[i]) return i;
    return -1;
}

/*  A sustained chord. The counterpart to pinkNoise: noise reads Tonal ~0, so a fixture
 *  built on it cannot exercise anything routed from Tonal at all. Deterministic, no source
 *  material needed. */
std::vector<float> chord(std::size_t n, double sr, double amp) {
    std::vector<float> out(n, 0.0f);
    static constexpr double kHz[] = {220.0, 277.18, 329.63, 440.0};
    for (double hz : kHz)
        for (std::size_t i = 0; i < n; ++i)
            out[i] += static_cast<float>(amp * std::sin(2.0 * kPi * hz * static_cast<double>(i) / sr));
    //  A slow tremolo so level and attack are not perfectly flat either.
    for (std::size_t i = 0; i < n; ++i)
        out[i] *= static_cast<float>(0.7 + 0.3 * std::sin(2.0 * kPi * 1.7 * static_cast<double>(i) / sr));
    return out;
}

/// Pink-ish noise, so a fixture can be made without hunting for source material.
std::vector<float> pinkNoise(std::size_t n, double amp, std::uint32_t seed = 22222) {
    std::vector<float> out(n);
    std::uint32_t x = seed;
    double b0 = 0, b1 = 0, b2 = 0;
    for (std::size_t i = 0; i < n; ++i) {
        x = x * 1664525u + 1013904223u;
        const double w = static_cast<double>(x) / 2147483648.0 - 1.0;
        b0 = 0.99765 * b0 + w * 0.0990460;
        b1 = 0.96300 * b1 + w * 0.2965164;
        b2 = 0.57000 * b2 + w * 1.0526913;
        out[i] = static_cast<float>((b0 + b1 + b2 + w * 0.1848) * amp);
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    //  Denormals flushed, as the plugin processes: a feedback path decays to the same zeros here.
    ScopedFlushDenormals flushDenormals;
    if (argc < 3) {
        usage();
        return argc < 2 ? 1 : 0;
    }

    std::string inPath, outPath, presetPath;
    double noiseSeconds = 0.0, toneSeconds = 0.0;
    int order = 3;
    double speedDeg = 90.0, widthDeg = 0.0, gainDb = 0.0;
    bool haveGenerator = false, printOnly = false;
    bool stereo = false;  // --stereo: the input has a side, so the input modes have something to place
    GeneratorType generator = GeneratorType::Lissajous;
    MovementMode mode = MovementMode::PingPong;
    std::vector<std::pair<std::string, double>> sets;
    std::vector<MatrixCell> modCells;
    std::vector<std::pair<ParamId, double>> params;
    bool useRegion = false;
    RegionKind regionKind = RegionKind::Spot;
    double bpm = 120.0;
    int blockSize = kBlock;
    std::string tracePath;

    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (a == "--preset")
            presetPath = next("--preset");
        else if (a == "--noise") {
            if (!parseDouble(next("--noise"), noiseSeconds)) return 2;
        } else if (a == "--tone") {
            if (!parseDouble(next("--tone"), toneSeconds)) return 2;
        } else if (a == "--order") {
            double v;
            if (!parseDouble(next("--order"), v)) return 2;
            order = static_cast<int>(v);
            //  Refused rather than clamped: the encoder clamps, and a clamped order above the
            //  ceiling would have written its extra channels as silence without a word.
            if (order < 0 || order > kMaxOrder) {
                std::fprintf(stderr, "--order must be 0..%d\n", kMaxOrder);
                return 2;
            }
        } else if (a == "--speed") {
            if (!parseDouble(next("--speed"), speedDeg)) return 2;
        } else if (a == "--width") {
            if (!parseDouble(next("--width"), widthDeg)) return 2;
        } else if (a == "--gain") {
            if (!parseDouble(next("--gain"), gainDb)) return 2;
        } else if (a == "--stereo")
            stereo = true;
        else if (a == "--print")
            printOnly = true;
        else if (a == "--bpm") {
            if (!parseDouble(next("--bpm"), bpm)) return 2;
        } else if (a == "--trace")
            tracePath = next("--trace");
        else if (a == "--block") {
            double v;
            if (!parseDouble(next("--block"), v)) return 2;
            blockSize = std::max(1, static_cast<int>(v));
        } else if (a == "--mod") {
            //  SRC:DEST=DEPTH
            const std::string spec = next("--mod");
            const auto colon = spec.find(':');
            const auto eq = spec.find('=');
            double depth = 0;
            if (colon == std::string::npos || eq == std::string::npos || eq < colon ||
                !parseDouble(spec.c_str() + eq + 1, depth)) {
                std::fprintf(stderr, "--mod expects SRC:DEST=DEPTH\n");
                return 2;
            }
            const std::string src = spec.substr(0, colon);
            const std::string dst = spec.substr(colon + 1, eq - colon - 1);
            const int slot = sourceByName(src);
            if (slot < 0) {
                std::fprintf(stderr, "unknown modulation source '%s'; known:", src.c_str());
                for (const char* nm : kSourceNames) std::fprintf(stderr, " %s", nm);
                std::fprintf(stderr, "\n");
                return 2;
            }
            const ParamId target = parameterByKey(dst);
            if (target == kNoParamId || destinationKind(target) == DestKind::NotModulatable) {
                std::fprintf(stderr, "'%s' is not a modulation destination; try:", dst.c_str());
                for (const auto& d : parameters())
                    if (destinationKind(d.id) != DestKind::NotModulatable)
                        std::fprintf(stderr, " %.*s", static_cast<int>(d.key.size()), d.key.data());
                std::fprintf(stderr, "\n");
                return 2;
            }
            modCells.push_back(
                MatrixCell{static_cast<MatrixTab>(slot / kSourcesPerTab), slot % kSourcesPerTab, target, depth});
        } else if (a == "--generator") {
            bool ok = false;
            generator = generatorByName(next("--generator"), ok);
            if (!ok) {
                std::fprintf(stderr, "unknown generator\n");
                return 2;
            }
            haveGenerator = true;
        } else if (a == "--mode") {
            bool ok = false;
            mode = modeByName(next("--mode"), ok);
            if (!ok) {
                std::fprintf(stderr, "unknown mode\n");
                return 2;
            }
        } else if (a == "--region") {
            const std::string kind = next("--region");
            constexpr const char* kinds[] = {"everywhere", "spot", "band", "sectors", "dots"};
            int found = -1;
            for (int k = 0; k < 5; ++k)
                if (kind == kinds[k]) found = k;
            if (found < 0) {
                std::fprintf(stderr, "unknown region kind %s\n", kind.c_str());
                return 2;
            }
            regionKind = static_cast<RegionKind>(found);
            useRegion = true;
        } else if (a == "--param") {
            const std::string kv = next("--param");
            const auto eq = kv.find('=');
            double v = 0;
            const ParamId id = eq == std::string::npos ? kNoParamId : parameterByKey(kv.substr(0, eq));
            if (id == kNoParamId || !parseDouble(kv.c_str() + eq + 1, v)) {
                std::fprintf(stderr, "--param expects KEY=VALUE with a parameter's key\n");
                return 2;
            }
            params.emplace_back(id, v);
        } else if (a == "--set") {
            const std::string kv = next("--set");
            const auto eq = kv.find('=');
            double v = 0;
            if (eq == std::string::npos || !parseDouble(kv.c_str() + eq + 1, v)) {
                std::fprintf(stderr, "--set expects NAME=VALUE\n");
                return 2;
            }
            sets.emplace_back(kv.substr(0, eq), v);
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        } else {
            positional.push_back(a);
        }
    }

    if (noiseSeconds > 0.0 || toneSeconds > 0.0) {
        if (noiseSeconds > 0.0 && toneSeconds > 0.0) {
            std::fprintf(stderr, "--noise and --tone are alternatives\n");
            return 2;
        }
        if (positional.size() != 1) {
            std::fprintf(stderr, "--noise/--tone take one output path\n");
            return 2;
        }
        outPath = positional[0];
    } else {
        if (positional.size() != 2) {
            usage();
            return 2;
        }
        inPath = positional[0];
        outPath = positional[1];
    }

    // ---- state -------------------------------------------------------------------------
    PluginState state{bambi::encodeParams()};
    if (!presetPath.empty()) {
        std::FILE* f = std::fopen(presetPath.c_str(), "rb");
        if (!f) {
            std::fprintf(stderr, "cannot open preset %s\n", presetPath.c_str());
            return 1;
        }
        std::string text;
        char buf[4096];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        std::fclose(f);
        const LoadResult r = loadState(bambi::Product::Encoder, bambi::encodeParams(), text, state);
        if (!r.ok) {
            std::fprintf(stderr, "preset: %s\n", r.message.c_str());
            return 1;
        }
        if (r.fromFuture) std::fprintf(stderr, "warning: %s\n", r.message.c_str());
        if (!haveGenerator) generator = state.trajectory.generator;
    }

    TrajectoryState traj = state.trajectory;
    if (traj.kind == TrajectoryKind::Parametric) {
        if (haveGenerator || presetPath.empty()) {
            traj.generator = generator;
            generatorDefaults(generator, traj.genParams);
        }
        for (const auto& [key, value] : sets) {
            const auto names = generatorParamNames(traj.generator);
            bool found = false;
            for (std::size_t i = 0; i < names.size(); ++i)
                if (names[i] == key) {
                    traj.genParams[i] = value;
                    found = true;
                }
            if (!found) {
                std::fprintf(stderr, "generator '%.*s' has no parameter '%s'; it has:",
                             static_cast<int>(name(traj.generator).size()), name(traj.generator).data(), key.c_str());
                for (auto nm : names) std::fprintf(stderr, " %.*s", static_cast<int>(nm.size()), nm.data());
                std::fprintf(stderr, "\n");
                return 2;
            }
        }
    } else if (!sets.empty()) {
        std::fprintf(stderr, "--set applies to parametric trajectories only\n");
        return 2;
    }

    Trajectory path;
    path.build(traj);
    if (path.empty()) {
        std::fprintf(stderr, "trajectory is empty\n");
        return 1;
    }

    // ---- input -------------------------------------------------------------------------
    AudioBuffer in;
    std::vector<float> mono;
    /*  The input's side, (L - R) / 2, for the modes that put a stereo input on the sphere.
        `--stereo` with the built-in noise makes it a second noise, unrelated to the mid, at half its
        level; a stereo file gives its own. Silence otherwise, and every mode is then `sum`. */
    std::vector<float> side;
    if (noiseSeconds > 0.0) {
        in.sampleRate = 48000;
        mono = pinkNoise(static_cast<std::size_t>(noiseSeconds * in.sampleRate), 0.12);
        if (stereo) side = pinkNoise(mono.size(), 0.06, 77777);
    } else if (toneSeconds > 0.0) {
        in.sampleRate = 48000;
        mono = chord(static_cast<std::size_t>(toneSeconds * in.sampleRate), in.sampleRate, 0.10);
    } else {
        std::string err;
        if (!readWav(inPath, in, &err)) {
            std::fprintf(stderr, "%s: %s\n", inPath.c_str(), err.c_str());
            return 1;
        }
        mono = in.mono();
        if (stereo && in.channels.size() >= 2) {
            side.resize(mono.size());
            for (std::size_t i = 0; i < side.size(); ++i) side[i] = (in.channels[0][i] - in.channels[1][i]) * 0.5f;
        }
    }
    side.resize(mono.size(), 0.0f);
    const std::size_t frames = mono.size();
    const double sr = in.sampleRate;

    const int nch = Encoder::numChannels(order);
    const double lengthDeg = path.lengthRad() * kRad2Deg;
    const double lapSeconds = speedDeg != 0.0 ? std::abs(lengthDeg / speedDeg) : 0.0;

    std::printf("in       %s\n", noiseSeconds > 0 ? "(pink noise)" : toneSeconds > 0 ? "(chord)" : inPath.c_str());
    std::printf("out      %s\n", outPath.c_str());
    std::printf("path     %.*s, %s, %.1f deg\n", static_cast<int>(name(traj.generator).size()),
                name(traj.generator).data(), path.closed() ? "closed" : "open", lengthDeg);
    std::printf("motion   %.1f deg/s%s -> %.2f s per lap\n", speedDeg,
                path.closed() ? ""
                              : (mode == MovementMode::PingPong ? ", ping-pong"
                                 : mode == MovementMode::Once   ? ", once"
                                                                : ", wrap"),
                lapSeconds);
    std::printf("encode   order %d, %d ch, width %.0f deg, gain %.1f dB\n", order, nch, widthDeg, gainDb);
    std::printf("audio    %.2f s at %.0f Hz\n", static_cast<double>(frames) / sr, sr);
    if (printOnly) return 0;

    // ---- render ------------------------------------------------------------------------
    AudioBuffer out;
    out.sampleRate = in.sampleRate;
    out.channels.assign(static_cast<std::size_t>(nch), std::vector<float>(frames, 0.0f));

    Encoder enc, encSide;  // fed the mid and the side, as the plugin's two are
    enc.prepare(order);
    encSide.prepare(order);

    /*  CLI flags set the parameter bases; modulation is added on top of them by the engine.
     *  Keeping one path rather than two means --speed 90 with no cells behaves identically
     *  to the plugin sitting at speed 90, instead of being a separate code path that can
     *  quietly disagree with it. */
    state.params[static_cast<std::size_t>(EncoderParam::MotionSpeed)] = static_cast<float>(speedDeg);
    state.params[static_cast<std::size_t>(EncoderParam::RenderWidth)] = static_cast<float>(widthDeg);
    state.params[static_cast<std::size_t>(EncoderParam::RenderGain)] = static_cast<float>(gainDb);
    state.params[static_cast<std::size_t>(EncoderParam::MotionMode)] = static_cast<float>(static_cast<int>(mode));
    //  A path's continuous settings are parameters: the shape's defaults and any --set go there, as a
    //  preset's already are; a --param of one of them, below, wins.
    if (traj.kind == TrajectoryKind::Parametric && (haveGenerator || presetPath.empty()))
        for (int i = 0; i < kMaxGenParams; ++i)
            if (const ParamId id = pathParamId(traj.generator, i); id != kNoParamId)
                state.params[static_cast<std::size_t>(id)] =
                    static_cast<float>(traj.genParams[static_cast<std::size_t>(i)]);
    for (const auto& [id, value] : params)
        state.params[static_cast<std::size_t>(id)] = clampToRange(id, static_cast<float>(value));
    if (traj.kind == TrajectoryKind::Parametric) {
        pathSettingsFrom(traj, state.params);
        path.build(traj);
    }
    if (useRegion) state.regions[0].shape.kind = regionKind;
    for (const auto& c : modCells) state.matrix.push_back(c);

    //  bambi-render reads no MIDI, so an envelope here can only fire from audio. A preset keeps its
    //  own triggers; a render built from flags uses audio ones, which also keeps the golden files
    //  where they were.
    if (presetPath.empty())
        for (auto& t : state.envTriggers) t.input = TriggerInput::Audio;

    FeatureBank feats;
    feats.prepare(sr, 2048, kControlHop);
    //  as the plugin sets it, a block: milliseconds at the boundary, seconds inside
    feats.setLevelRelease(static_cast<double>(state.params[static_cast<std::size_t>(EncoderParam::LevelRelease)]) *
                          0.001);
    ModulationEngine mod;
    mod.useManifest(encodeMod());
    mod.prepare(sr);
    mod.setState(state);
    mod.reset();
    //  The plugin's control step, not one of this tool's own (bambi/encode/control.hpp): what is
    //  rendered and hashed here is what the plugin does each step.
    EncoderControl control;
    control.reset();

    const Vec3 centre = pathCentre(path.points());
    const double controlDt = static_cast<double>(kControlHop) / sr;

    Transport tp;
    tp.playing = true;
    tp.bpm = bpm;
    //  A step's song position as the plugin's grid gives it: from where playback began, in samples.
    BeatClock beats;
    beats.observe(0, 0.0, bpm, sr);
    double trimDb = 0.0;

    std::vector<float*> ptrs(static_cast<std::size_t>(nch));

    /*  Audio is walked in segments bounded by the control grid, never by the block size, so
     *  a render with --block 64 and one with --block 1024 produce identical samples. That
     *  is the offline-bounce requirement, and it is a property of this loop's shape rather
     *  than something to be verified afterwards. */
    std::FILE* trace = nullptr;
    if (!tracePath.empty()) {
        trace = std::fopen(tracePath.c_str(), "wb");
        if (trace == nullptr) {
            std::fprintf(stderr, "cannot write trace %s\n", tracePath.c_str());
            return 1;
        }
        std::fprintf(trace,
                     "t,level,attack,tonal,low,mid,high,"
                     "speed,displace,width,gain,yaw,pitch,roll,extent,"
                     "s,az,el%s\n",
                     useRegion ? ",region" : "");
    }

    std::size_t pos = 0;
    int sinceControl = 0;
    while (pos < frames) {
        if (sinceControl == 0) {
            //  Features are fed before this point in every previous iteration, so the values
            //  read here describe audio already heard. Causal, and zero at the very start.
            std::array<double, kNumFeatures> f{};
            for (int i = 0; i < kNumFeatures; ++i)
                f[static_cast<std::size_t>(i)] = feats.value(static_cast<Feature>(i));
            tp.ppq = beats.ppqAt(static_cast<std::int64_t>(pos));
            ControlInput step;
            step.self = f;
            step.transport = tp;
            step.dt = controlDt;
            step.path = &path;
            step.centre = centre;
            step.shape = &traj;  // the path as set; a parametric one is rebuilt as the engine moves it
            if (useRegion) step.region = &state.regions[0];
            step.order = order;
            step.stereoInput = stereo;
            const ControlFrame frame = control.step(mod, state.params, step);
            //  Ramped over the whole control period, not over whatever segment comes next.
            applyFrame(enc, encSide, frame);
            trimDb = frame.trimDb;
            const double s01 = frame.s;

            if (trace != nullptr) {
                const Vec3 p = frame.position;
                std::fprintf(trace,
                             "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,"
                             "%.4f,%.6f,%.4f,%.4f,%.4f,%.4f,%.4f,%.6f,"
                             "%.6f,%.4f,%.4f",
                             static_cast<double>(pos) / sr, f[0], f[1], f[2], f[3], f[4], f[5],
                             mod.destination(EncoderParam::MotionSpeed), mod.destination(EncoderParam::MotionDisplace),
                             mod.destination(EncoderParam::RenderWidth), mod.destination(EncoderParam::RenderGain),
                             mod.destination(EncoderParam::TransformYaw), mod.destination(EncoderParam::TransformPitch),
                             mod.destination(EncoderParam::TransformRoll),
                             mod.destination(EncoderParam::TransformExtent), s01, azimuth(p) * kRad2Deg,
                             elevation(p) * kRad2Deg);
                if (useRegion) std::fprintf(trace, ",%.6f", frame.regionValue);
                std::fprintf(trace, "\n");
            }
        }

        const int n = static_cast<int>(std::min<std::size_t>(
            {static_cast<std::size_t>(blockSize), static_cast<std::size_t>(kControlHop - sinceControl), frames - pos}));

        //  Input trim, where both the detectors and the encoder see it -- as the plugin applies it.
        if (trimDb != 0.0) {
            const auto g = static_cast<float>(std::pow(10.0, trimDb / 20.0));
            for (int i = 0; i < n; ++i) mono[pos + static_cast<std::size_t>(i)] *= g;
            for (int i = 0; i < n; ++i) side[pos + static_cast<std::size_t>(i)] *= g;
        }
        feats.process(mono.data() + pos, n);
        for (int c = 0; c < nch; ++c)
            ptrs[static_cast<std::size_t>(c)] = out.channels[static_cast<std::size_t>(c)].data() + pos;
        enc.process(mono.data() + pos, ptrs, n);
        if (!encSide.silent()) encSide.process(side.data() + pos, ptrs, n);  // not run in `sum`: the goldens' path

        pos += static_cast<std::size_t>(n);
        sinceControl = (sinceControl + n) % kControlHop;
    }

    if (trace != nullptr) {
        std::fclose(trace);
        std::printf("trace    %s\n", tracePath.c_str());
    }

    std::string err;
    if (!writeWav(outPath, out, &err)) {
        std::fprintf(stderr, "%s: %s\n", outPath.c_str(), err.c_str());
        return 1;
    }
    std::printf("wrote    %d channels, %.2f s\n", nch, static_cast<double>(frames) / sr);
    return 0;
}
