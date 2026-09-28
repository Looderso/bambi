// SPDX-License-Identifier: GPL-3.0-or-later
//
// bambi-echo-render — Echo's engine, offline, with no plugin and no host.
//
// What bambi-render is to the encoder: the whole chain from settings through the control step to
// audio, rendered and hashed, so that a change in what Echo does is a changed golden and a render
// that depends on the host's block size is a failed check. It links bambi-echo and nothing of the
// encoder, so neither plugin's goldens move when the other changes.
//
// The input is made here, from a generator written out below, so no audio lives in the repo: a few
// voices at fixed directions, each a run of short noise bursts -- a skew or a send cannot be judged
// on one source -- or a single impulse.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "Changes.hpp"
#include "bambi/echo/control.hpp"
#include "bambi/echo/engine.hpp"
#include "bambi/io/wav.hpp"
#include "bambi/math/denormals.hpp"
#include "bambi/math/sh.hpp"

using namespace bambi;

namespace {

void usage() {
    std::fputs(
        "bambi-echo-render [options] out.wav\n"
        "  --order N            ambisonic order, 1..7 (3)\n"
        "  --seconds S          length (4)\n"
        "  --bpm B              tempo (120)\n"
        "  --voices N           N voices of noise bursts round the listener (4)\n"
        "  --impulse AZ,EL      one impulse at a direction, in degrees, instead of voices\n"
        "  --set PATH=VALUE     a setting, repeatable: tapK.on|synced|steps|offset_steps|swing|ms|offset_ms|\n"
        "                       level|feedback|az|el|spin|skew|blur|low_cut|high_cut  (K = 1..4), and\n"
        "                       send.kind (everywhere|spot|band) send.side (inside|outside) send.amount\n"
        "                       send.yaw|pitch|roll|size|softness|band_elevation|thickness, and bpm\n"
        "  --at S PATH=VALUE    the same, taking effect at the first control step at or after S seconds;\n"
        "                       PATH may be `reset`, a play start or locate\n"
        "  --block N            host block size (512). The result must not depend on it\n"
        "  --trace FILE         a CSV, one row a control step: t, energy, the four periods, clamped\n",
        stderr);
}

bool apply(EchoSettings& s, double& bpm, const std::string& path, const std::string& value) {
    const double v = std::atof(value.c_str());
    if (path == "bpm") {
        bpm = v;
        return true;
    }
    if (path.starts_with("tap") && path.size() > 5 && path[4] == '.') {
        const int k = path[3] - '1';
        if (k < 0 || k >= kEchoTaps) return false;
        EchoTapSettings& t = s.taps[static_cast<std::size_t>(k)];
        const std::string key = path.substr(5);
        if (key == "on")
            t.on = v != 0.0;
        else if (key == "synced")
            t.timing.synced = v != 0.0;
        else if (key == "steps")
            t.timing.steps = static_cast<int>(v);
        else if (key == "offset_steps")
            t.timing.offsetSteps = static_cast<int>(v);
        else if (key == "swing")
            t.timing.swing = v;
        else if (key == "ms")
            t.timing.ms = v;
        else if (key == "offset_ms")
            t.timing.offsetMs = v;
        else if (key == "level")
            t.levelDb = v;
        else if (key == "feedback")
            t.feedbackDb = v;
        else if (key == "az")
            t.axisAzimuthDeg = v;
        else if (key == "el")
            t.axisElevationDeg = v;
        else if (key == "spin")
            t.spinDeg = v;
        else if (key == "skew")
            t.skew = v;
        else if (key == "blur")
            t.blurDeg = v;
        else if (key == "low_cut")
            t.lowCutHz = v;
        else if (key == "high_cut")
            t.highCutHz = v;
        else
            return false;
        return true;
    }
    if (path == "send.kind") {
        if (value == "everywhere")
            s.sendShape.kind = RegionKind::Everywhere;
        else if (value == "spot")
            s.sendShape.kind = RegionKind::Spot;
        else if (value == "band")
            s.sendShape.kind = RegionKind::Band;
        else
            return false;
        return true;
    }
    if (path == "send.side") {
        s.sendSide = value == "outside" ? RegionSide::Outside : RegionSide::Inside;
        return value == "outside" || value == "inside";
    }
    if (path == "dry") {
        s.dryDb = v;
        return true;
    }  // the output stage, as reverb-render names it
    if (path == "wet") {
        s.wetDb = v;
        return true;
    }
    if (path == "send.amount") {
        s.sendAmount = v;
        return true;
    }
    if (path == "send.yaw") {
        s.send.yaw = v;
        return true;
    }
    if (path == "send.pitch") {
        s.send.pitch = v;
        return true;
    }
    if (path == "send.roll") {
        s.send.roll = v;
        return true;
    }
    if (path == "send.size") {
        s.send.size = v;
        return true;
    }
    if (path == "send.softness") {
        s.send.softness = v;
        return true;
    }
    if (path == "send.band_elevation") {
        s.send.bandElevation = v;
        return true;
    }
    if (path == "send.thickness") {
        s.send.thickness = v;
        return true;
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    //  Denormals flushed, as the plugin processes: a feedback path decays to the same zeros here.
    ScopedFlushDenormals flushDenormals;
    int order = 3, voices = 4, blockSize = 512;
    double seconds = 4.0, bpm = 120.0, impulseAz = 0.0, impulseEl = 0.0;
    bool impulse = false;
    std::string outPath, tracePath;
    EchoSettings settings = EchoSettings::defaults();
    std::vector<tools::Change> changes;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", flag);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (a == "--order")
            order = std::atoi(next("--order").c_str());
        else if (a == "--seconds")
            seconds = std::atof(next("--seconds").c_str());
        else if (a == "--bpm")
            bpm = std::atof(next("--bpm").c_str());
        else if (a == "--voices")
            voices = std::atoi(next("--voices").c_str());
        else if (a == "--block")
            blockSize = std::atoi(next("--block").c_str());
        else if (a == "--trace")
            tracePath = next("--trace");
        else if (a == "--impulse") {
            const std::string v = next("--impulse");
            if (std::sscanf(v.c_str(), "%lf,%lf", &impulseAz, &impulseEl) != 2) {
                std::fprintf(stderr, "--impulse expects AZ,EL\n");
                return 2;
            }
            impulse = true;
        } else if (a == "--set") {
            std::string path, value;
            if (!tools::splitSetting(next("--set"), path, value) || !apply(settings, bpm, path, value)) {
                std::fprintf(stderr, "--set: no such setting\n");
                return 2;
            }
        } else if (a == "--at") {
            tools::Change c;
            const double at = std::atof(next("--at").c_str());
            if (!tools::parseChange(at, next("--at"), c)) {
                std::fprintf(stderr, "--at expects S PATH=VALUE or S reset\n");
                return 2;
            }
            changes.push_back(c);
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        } else
            outPath = a;
    }
    if (outPath.empty() || order < 1 || order > 7 || blockSize < 1 || seconds <= 0.0) {
        usage();
        return 2;
    }

    const double fs = 48000.0;
    const int C = numChannels(order);
    const auto frames = static_cast<std::size_t>(seconds * fs);

    // ---- the input field, interleaved ---------------------------------------------------------
    std::vector<float> in(frames * static_cast<std::size_t>(C), 0.0f);
    std::vector<double> y(static_cast<std::size_t>(C));
    if (impulse) {
        shSN3D(fromAzEl(impulseAz * kDeg2Rad, impulseEl * kDeg2Rad), order, y);
        for (int c = 0; c < C; ++c)
            in[static_cast<std::size_t>(c)] = static_cast<float>(0.5 * y[static_cast<std::size_t>(c)]);
    } else {
        unsigned seed = 0x2545F491u;
        for (int v = 0; v < std::max(1, voices); ++v) {
            //  round the horizon, alternately a little above and below it
            shSN3D(fromAzEl(2.0 * kPi * v / std::max(1, voices), (v % 2 == 0 ? 15.0 : -20.0) * kDeg2Rad), order, y);
            const std::size_t every = static_cast<std::size_t>(fs * (0.61 + 0.17 * v)),
                              length = static_cast<std::size_t>(fs * 0.03);
            for (std::size_t start = static_cast<std::size_t>(fs * 0.05 * v); start + length < frames; start += every)
                for (std::size_t k = 0; k < length; ++k) {
                    seed = seed * 1664525u + 1013904223u;
                    const double noise = (static_cast<int>(seed >> 8) % 20001 - 10000) / 10000.0;
                    const double env = std::sin(kPi * static_cast<double>(k) / static_cast<double>(length));
                    const double s = 0.2 * noise * env * env;
                    for (int c = 0; c < C; ++c)
                        in[(start + k) * static_cast<std::size_t>(C) + static_cast<std::size_t>(c)] +=
                            static_cast<float>(s * y[static_cast<std::size_t>(c)]);
                }
        }
    }

    // ---- render -------------------------------------------------------------------------------
    EchoEngine engine;
    engine.prepare(order, fs);
    EchoResolver resolver;
    std::vector<float> out(in.size(), 0.0f);

    std::FILE* trace = nullptr;
    if (!tracePath.empty()) {
        trace = std::fopen(tracePath.c_str(), "wb");
        if (trace == nullptr) {
            std::fprintf(stderr, "cannot write %s\n", tracePath.c_str());
            return 1;
        }
        std::fprintf(trace, "t,energy,p1,p2,p3,p4,clamped\n");
    }

    /*  Walked in segments bounded by the control grid and by the host's block, never by the block
     *  alone: a frame is decided on the grid, from the timeline, so --block 1 and --block 1024 give the
     *  same samples. That is checked, not assumed (tools/golden.sh). */
    std::size_t pos = 0;
    tools::ChangeSchedule schedule(std::move(changes));
    EchoFrame frame;
    double hopEnergy = 0.0;
    std::size_t hopStart = 0;
    const auto closeHop = [&] {
        if (trace != nullptr && pos > hopStart)
            std::fprintf(trace, "%.6f,%.9e,%d,%d,%d,%d,%lld\n", static_cast<double>(hopStart) / fs, hopEnergy,
                         frame.taps[0].periodSamples, frame.taps[1].periodSamples, frame.taps[2].periodSamples,
                         frame.taps[3].periodSamples, engine.clamped());
    };
    while (pos < frames) {
        if (pos % kEchoHop == 0) {
            closeHop();
            hopEnergy = 0.0;
            hopStart = pos;
            const tools::Change* refused = schedule.applyDue(fs, pos, [&](const tools::Change& c) {
                if (!c.reset()) return apply(settings, bpm, c.path, c.value);
                engine.reset();
                resolver.reset();
                return true;
            });
            if (refused != nullptr) {
                std::fprintf(stderr, "--at: no such setting %s\n", refused->path.c_str());
                return 2;
            }
            frame = resolver.resolve(settings, bpm, fs, order);
            engine.setFrame(frame);
        }
        const std::size_t toHop = kEchoHop - pos % kEchoHop,
                          toBlock = static_cast<std::size_t>(blockSize) - pos % static_cast<std::size_t>(blockSize);
        const auto n = static_cast<int>(std::min({toHop, toBlock, frames - pos}));
        engine.process(in.data() + pos * static_cast<std::size_t>(C), out.data() + pos * static_cast<std::size_t>(C),
                       n);
        for (std::size_t k = pos * static_cast<std::size_t>(C);
             k < (pos + static_cast<std::size_t>(n)) * static_cast<std::size_t>(C); ++k)
            hopEnergy += static_cast<double>(out[k]) * out[k];
        pos += static_cast<std::size_t>(n);
    }
    closeHop();
    if (trace != nullptr) std::fclose(trace);

    AudioBuffer wav;
    wav.sampleRate = static_cast<int>(fs);
    wav.channels.assign(static_cast<std::size_t>(C), std::vector<float>(frames));
    for (std::size_t f = 0; f < frames; ++f)
        for (int c = 0; c < C; ++c)
            wav.channels[static_cast<std::size_t>(c)][f] =
                out[f * static_cast<std::size_t>(C) + static_cast<std::size_t>(c)];
    std::string err;
    if (!writeWav(outPath, wav, &err)) {
        std::fprintf(stderr, "%s: %s\n", outPath.c_str(), err.c_str());
        return 1;
    }
    std::printf("wrote    %d channels, %.2f s, clamped %lld\n", C, seconds, engine.clamped());
    return 0;
}
