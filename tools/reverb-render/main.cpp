// SPDX-License-Identifier: GPL-3.0-or-later
//
// bambi-reverb-render — Reverb's engine, offline, with no plugin and no host.
//
// What bambi-echo-render is to Echo. It links bambi-reverb and nothing of the encoder or of Echo,
// so no plugin's goldens move when another changes.
//
// The input is made here, from a generator written out below, so no audio lives in the repo: a few
// voices at fixed directions, each a run of short noise bursts — a send or a return cannot be judged
// on one source — or a single impulse, which is how a room is read.
//
// Settings are in degrees here and become radians in one place, `resolveRegion`, as the convention
// asks. That conversion and the defaults below are standing in for a `ReverbControl` that does not
// exist yet: Reverb's parameter list is not approved, so this tool names its own settings and a
// later ReverbControl absorbs them rather than copying them.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "Changes.hpp"
#include "bambi/io/wav.hpp"
#include "bambi/math/denormals.hpp"
#include "bambi/math/sh.hpp"
#include "bambi/region/shape.hpp"
#include "bambi/reverb/control.hpp"

using namespace bambi;

namespace {

void usage() {
    std::fputs(
        "bambi-reverb-render [options] out.wav\n"
        "  --order N            ambisonic order, 1..7 (3)\n"
        "  --seconds S          length (4)\n"
        "  --voices N           N voices of noise bursts round the listener (4)\n"
        "  --impulse AZ,EL      one impulse at a direction, in degrees, instead of voices\n"
        "  --set PATH=VALUE     a setting, repeatable. The control layer's, in its own units\n"
        "                       (reverb/control.hpp): preset(ambience|room|chamber|hall|large_hall|\n"
        "                       cathedral) room.size room.shape(room|hall|tall) room.decay room.tone\n"
        "                       room.roughness distance wet dry pre_delay_trim(ms) low_cut high_cut\n"
        "                       quality(efficient|realistic) render_quality(same|realistic) offline\n"
        "                       send.* and return.*: kind(everywhere|spot|band|sectors|dots)\n"
        "                       side(inside|outside) amount yaw pitch roll size softness\n"
        "                       band_elevation thickness sectors fill dots dot_size -- DEGREES\n"
        "                       and two ENGINE overrides no control reaches: lines, tail_order\n"
        "  --at S PATH=VALUE    the same, taking effect at the first control step at or after S\n"
        "                       seconds; PATH may be `reset`\n"
        "  --block N            host block size (512). The result must not depend on it\n"
        "  --trace FILE         a CSV, one row a control step: t, energy, rt60, mixing time,\n"
        "                       pre-delay, the early, tail and dry gains, the lines\n",
        stderr);
}

/*  The tool is set in ReverbSettings, the same struct the plugin will be (reverb/control.hpp): the
 *  units, the ranges and the degrees-to-radians step are the control layer's, not this file's. What
 *  is left here is the flags, and two engine overrides -- `lines` and `tail_order` -- which no user
 *  control reaches and which exist so a golden can pin the engine at a setting the quality switch
 *  does not offer. */
struct Settings {
    ReverbSettings user;
    int lines{0};       ///< 0: whatever quality asks for
    int tailOrder{-1};  ///< -1: the engine's own
    bool offline{false};
};

bool applyRegion(ReverbRegionSettings& r, const std::string& key, const std::string& value, double v) {
    if (key == "kind") {
        if (value == "everywhere")
            r.shape.kind = RegionKind::Everywhere;
        else if (value == "spot")
            r.shape.kind = RegionKind::Spot;
        else if (value == "band")
            r.shape.kind = RegionKind::Band;
        else if (value == "sectors")
            r.shape.kind = RegionKind::Sectors;
        else if (value == "dots")
            r.shape.kind = RegionKind::Dots;
        else
            return false;
        return true;
    }
    if (key == "side") {
        if (value != "inside" && value != "outside") return false;
        r.side = value == "outside" ? RegionSide::Outside : RegionSide::Inside;
        return true;
    }
    if (key == "amount") {
        r.amount = v;
        return true;
    }
    if (key == "yaw") {
        r.deg.yaw = v;
        return true;
    }
    if (key == "pitch") {
        r.deg.pitch = v;
        return true;
    }
    if (key == "roll") {
        r.deg.roll = v;
        return true;
    }
    if (key == "yaw_rate") {
        r.rates.yaw = v;
        return true;
    }
    if (key == "pitch_rate") {
        r.rates.pitch = v;
        return true;
    }
    if (key == "roll_rate") {
        r.rates.roll = v;
        return true;
    }
    if (key == "size") {
        r.deg.size = v;
        return true;
    }
    if (key == "softness") {
        r.deg.softness = v;
        return true;
    }
    if (key == "band_elevation") {
        r.deg.bandElevation = v;
        return true;
    }
    if (key == "thickness") {
        r.deg.thickness = v;
        return true;
    }
    if (key == "fill") {
        r.deg.fill = v;
        return true;
    }
    if (key == "dot_size") {
        r.deg.dotSize = v;
        return true;
    }
    if (key == "sectors") {
        r.shape.sectors = static_cast<int>(v);
        return true;
    }
    if (key == "dots") {
        r.shape.dots = static_cast<int>(v);
        return true;
    }
    return false;
}

bool apply(Settings& s, const std::string& path, const std::string& value) {
    const double v = std::atof(value.c_str());
    ReverbSettings& u = s.user;
    if (path == "room.size") {
        u.sizeMetres = v;
        return true;
    }
    if (path == "room.shape") {
        if (value == "room")
            u.shape = RoomShape::Room;
        else if (value == "hall")
            u.shape = RoomShape::Hall;
        else if (value == "tall")
            u.shape = RoomShape::Tall;
        else
            return false;
        return true;
    }
    if (path == "room.decay") {
        u.decaySeconds = v;
        return true;
    }
    if (path == "room.tone") {
        u.tone = v;
        return true;
    }
    if (path == "room.roughness") {
        u.roughness = v;
        return true;
    }
    if (path == "distance") {
        u.distanceMetres = v;
        return true;
    }
    if (path == "level" || path == "wet") {
        u.wetDb = v;
        return true;
    }  // `level` kept: the goldens use it
    if (path == "dry") {
        u.dryDb = v;
        return true;
    }
    if (path == "pre_delay_trim") {
        u.preDelayTrimMs = v;
        return true;
    }
    if (path == "low_cut") {
        u.lowCutHz = v;
        return true;
    }
    if (path == "high_cut") {
        u.highCutHz = v;
        return true;
    }
    if (path == "quality") {
        if (value == "efficient")
            u.quality = ReverbQuality::Efficient;
        else if (value == "realistic")
            u.quality = ReverbQuality::Realistic;
        else
            return false;
        return true;
    }
    if (path == "render_quality") {
        if (value == "same")
            u.renderQuality = RenderQuality::Same;
        else if (value == "realistic")
            u.renderQuality = RenderQuality::Realistic;
        else
            return false;
        return true;
    }
    if (path == "preset") {
        static const std::array<std::pair<const char*, ReverbPreset>, 6> kNames{
            {{"ambience", ReverbPreset::Ambience},
             {"room", ReverbPreset::Room},
             {"chamber", ReverbPreset::Chamber},
             {"hall", ReverbPreset::Hall},
             {"large_hall", ReverbPreset::LargeHall},
             {"cathedral", ReverbPreset::Cathedral}}};
        for (const auto& n : kNames)
            if (value == n.first) {
                u = ReverbSettings::preset(n.second);
                return true;
            }
        return false;
    }
    if (path == "lines") {
        s.lines = static_cast<int>(v);
        return true;
    }
    if (path == "tail_order") {
        s.tailOrder = static_cast<int>(v);
        return true;
    }
    if (path == "offline") {
        s.offline = v != 0.0;
        return true;
    }
    if (path.starts_with("send.")) return applyRegion(u.send, path.substr(5), value, v);
    if (path.starts_with("return.")) return applyRegion(u.returnTo, path.substr(7), value, v);
    return false;
}

/*  The control step, and then the engine-only overrides a golden may pin. Through the resolver, so
 *  a region turning at a rate turns here as it does in a plugin -- and so `--at reset` puts the
 *  turn back with the engine, which is what makes a render repeatable. */
ReverbFrame frameOf(ReverbResolver& r, const Settings& s, double sampleRate) {
    ReverbFrame f = r.resolve(s.user, s.offline, sampleRate);
    if (s.lines > 0) f.tailLines = s.lines;
    if (s.tailOrder >= 0) f.tailOrder = s.tailOrder;
    return f;
}

}  // namespace

int main(int argc, char** argv) {
    //  Denormals flushed, as the plugin processes: a feedback path decays to the same zeros here.
    ScopedFlushDenormals flushDenormals;
    int order = 3, voices = 4, blockSize = 512;
    double seconds = 4.0, impulseAz = 0.0, impulseEl = 0.0;
    bool impulse = false;
    std::string outPath, tracePath;
    Settings settings;
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
            if (!tools::splitSetting(next("--set"), path, value) || !apply(settings, path, value)) {
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
            shSN3D(fromAzEl(2.0 * kPi * v / std::max(1, voices), (v % 2 == 0 ? 15.0 : -20.0) * kDeg2Rad), order, y);
            const auto every = static_cast<std::size_t>(fs * (0.61 + 0.17 * v)),
                       length = static_cast<std::size_t>(fs * 0.03);
            for (std::size_t start = static_cast<std::size_t>(fs * 0.05 * v); start + length < frames; start += every)
                for (std::size_t k = 0; k < length; ++k) {
                    seed = seed * 1664525u + 1013904223u;
                    const double noise = (static_cast<int>(seed >> 8) % 20001 - 10000) / 10000.0;
                    const double env = std::sin(kPi * static_cast<double>(k) / static_cast<double>(length));
                    const double sample = 0.2 * noise * env * env;
                    for (int c = 0; c < C; ++c)
                        in[(start + k) * static_cast<std::size_t>(C) + static_cast<std::size_t>(c)] +=
                            static_cast<float>(sample * y[static_cast<std::size_t>(c)]);
                }
        }
    }

    // ---- render -------------------------------------------------------------------------------
    ReverbEngine engine;
    engine.prepare(order, fs);
    std::vector<float> out(in.size(), 0.0f);

    std::FILE* trace = nullptr;
    if (!tracePath.empty()) {
        trace = std::fopen(tracePath.c_str(), "wb");
        if (trace == nullptr) {
            std::fprintf(stderr, "cannot write %s\n", tracePath.c_str());
            return 1;
        }
        std::fprintf(trace, "t,energy,rt_mid,mixing_ms,pre_delay_ms,early_gain,tail_gain,dry_gain,lines\n");
    }

    /*  Walked in segments bounded by the control grid and by the host's block, never by the block
     *  alone, so --block 1 and --block 1024 give the same samples. Checked, not assumed. */
    std::size_t pos = 0;
    tools::ChangeSchedule schedule(std::move(changes));
    double hopEnergy = 0.0;
    std::size_t hopStart = 0;
    const auto closeHop = [&] {
        if (trace != nullptr && pos > hopStart) {
            const ReverbBalance& b = engine.balance();
            std::fprintf(trace, "%.6f,%.9e,%.6f,%.4f,%.4f,%.9e,%.9e,%.9e,%d\n", static_cast<double>(hopStart) / fs,
                         hopEnergy, engine.room().rtMid, engine.room().mixingTime * 1000.0, b.preDelaySeconds * 1000.0,
                         b.earlyGain, b.tailGain, b.dryGain, engine.lines());
        }
    };
    ReverbResolver resolver;
    while (pos < frames) {
        if (pos % kReverbHop == 0) {
            closeHop();
            hopEnergy = 0.0;
            hopStart = pos;
            const tools::Change* refused = schedule.applyDue(fs, pos, [&](const tools::Change& c) {
                if (!c.reset()) return apply(settings, c.path, c.value);
                engine.reset();
                resolver.reset();
                return true;
            });
            if (refused != nullptr) {
                std::fprintf(stderr, "--at: no such setting %s\n", refused->path.c_str());
                return 2;
            }
            engine.set(frameOf(resolver, settings, fs));
        }
        const std::size_t toHop = kReverbHop - pos % kReverbHop,
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
    std::printf("wrote    %d channels, %.2f s, RT60 mid %.3f s\n", C, seconds, engine.room().rtMid);
    return 0;
}
