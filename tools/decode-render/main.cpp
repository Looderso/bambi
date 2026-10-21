// SPDX-License-Identifier: GPL-3.0-or-later
//
// bambi-decode-render — the Decoder's engine, offline, with no plugin and no host.
//
// What bambi-echo-render is to Echo. It links bambi-decode and nothing of the encoder or the effects,
// so no plugin's goldens move when another changes. A field goes in; the bus the outputs play on comes
// out, every channel of it, with a control trace beside it.
//
// The field is made here, from a generator written out below, so no audio lives in the repo: a few
// voices of noise at fixed directions round the listener, or one impulse. Its order may be narrower
// than the bus's, as an order-1 field in a third-order track is; the channels past it are silent.
//
// The listening situation is the plugin's own state (DecoderState) and the host's parameters
// (DecoderFrame), set by name below; the heads are the ones the plugin carries, read from
// core/data/decode. Everything the state asks for is built before the first block, as the plugin
// builds it on prepare, and a change of state mid-render is built at the step it lands on.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "Changes.hpp"
#include "bambi/decode/engine.hpp"
#include "bambi/decode/headmodelfile.hpp"
#include "bambi/decode/layouts.hpp"
#include "bambi/decode/orientation.hpp"
#include "bambi/io/wav.hpp"
#include "bambi/math/denormals.hpp"
#include "bambi/math/sh.hpp"

using namespace bambi;

namespace {

constexpr int kStep = 256;  ///< the control grid: settings change, and the trace takes a row, every this many frames

void usage() {
    std::fputs(
        "bambi-decode-render [options] out.wav\n"
        "  --order N            the bus's ambisonic order, 0..7 (3): what the decode is built for\n"
        "  --field-order N      the field's own order, at most the bus's (the bus's); the channels past it\n"
        "                       are silent\n"
        "  --seconds S          length (3)\n"
        "  --voices N           N voices of noise round the listener (3)\n"
        "  --impulse AZ,EL      one impulse at a direction, in degrees, instead of voices\n"
        "  --channels N         the output bus's width (what the outputs need, two at least)\n"
        "  --set PATH=VALUE     a setting, repeatable.\n"
        "                       The state's: headphones(0|1) headphone_channel speakers(0|1)\n"
        "                       first_channel hrtf(model|ku100) head_size ear_shift through_layout(0|1)\n"
        "                       weighting(basic|max_re) slice(0|1) distance(0|1) static(0|1)\n"
        "                       tracker_turns_scene(0|1) layout(a BS.2051 name, \"4+7+0\")\n"
        "                       speaker(AZ,EL,R: one more loudspeaker, on the next channel) matrix(a SOFA\n"
        "                       FIR-E file) matrix_norm(file|sn3d|n3d)\n"
        "                       The host's: yaw pitch roll (degrees) yaw_rate pitch_rate roll_rate\n"
        "                       (degrees a second) level (dB) rates(0|1: they integrate)\n"
        "                       The tracker's head: head.yaw head.pitch head.roll (degrees)\n"
        "  --at S PATH=VALUE    the same, taking effect at the first control step at or after S\n"
        "                       seconds; PATH may be `reset`, a start or a locate: the rates' turn to zero\n"
        "  --block N            host block size (512). The result must not depend on it\n"
        "  --heads DIR          where head.json and ku100.json are (core/data/decode)\n"
        "  --trace FILE         a CSV, one row a control step: t, the energy of every output, of the\n"
        "                       headphones' pair and of the loudspeakers, the head's and the scene's\n"
        "                       yaw and pitch, the loudspeaker outputs playing\n",
        stderr);
}

struct Settings {
    DecoderState state;
    DecoderFrame controls;
    double headYaw{0.0}, headPitch{0.0}, headRoll{0.0};  ///< the tracker's, degrees
};

bool flag(const std::string& value, bool& out) {
    if (value != "0" && value != "1") return false;
    out = value == "1";
    return true;
}

/// Sets one setting; `stateChanged` says whether the engine must build again.
bool apply(Settings& s, const std::string& path, const std::string& value, bool& stateChanged) {
    const double v = std::atof(value.c_str());
    DecoderState& d = s.state;
    DecoderFrame& c = s.controls;
    stateChanged = true;
    if (path == "headphones") return flag(value, d.headphonesPlay);
    if (path == "speakers") return flag(value, d.speakersPlay);
    if (path == "through_layout") return flag(value, d.throughLayout);
    if (path == "slice") return flag(value, d.pairSlice);
    if (path == "distance") return flag(value, d.distanceCompensation);
    if (path == "static") return flag(value, d.sceneStatic);
    if (path == "tracker_turns_scene") return flag(value, d.trackerTurnsScene);
    if (path == "headphone_channel") {
        d.headphoneChannel = std::atoi(value.c_str());
        return d.headphoneChannel >= 1;
    }
    if (path == "first_channel") {
        d.speakerFirstChannel = std::atoi(value.c_str());
        return d.speakerFirstChannel >= 1;
    }
    if (path == "hrtf") {
        if (value != "model" && value != "ku100") return false;
        d.hrtf = value == "ku100" ? 1 : 0;
        return true;
    }
    if (path == "head_size") {
        d.headSize = static_cast<float>(v);
        return true;
    }
    if (path == "ear_shift") {
        d.earShift = static_cast<float>(v);
        return true;
    }
    if (path == "weighting") {
        if (value != "basic" && value != "max_re") return false;
        d.weighting = value == "max_re" ? 1 : 0;
        return true;
    }
    if (path == "layout") {
        const Layout* l = bs2051Layout(value);
        if (l == nullptr) return false;
        d.layout = l->speakers;
        d.layoutName = value;
        return true;
    }
    if (path == "speaker") {
        Loudspeaker one;
        if (std::sscanf(value.c_str(), "%lf,%lf,%lf", &one.azimuthDeg, &one.elevationDeg, &one.radiusM) != 3)
            return false;
        int channel = 0;
        for (const Loudspeaker& l : d.layout) channel = std::max(channel, l.channel);
        one.channel = channel + 1;
        d.layout.push_back(one);
        return true;
    }
    if (path == "matrix") {
        d.matrixFile = value;
        return true;
    }
    if (path == "matrix_norm") {
        if (value == "file")
            d.matrixNormalization = 0;
        else if (value == "sn3d")
            d.matrixNormalization = 1;
        else if (value == "n3d")
            d.matrixNormalization = 2;
        else
            return false;
        return true;
    }
    stateChanged = false;
    if (path == "yaw")
        c.yawDeg = v;
    else if (path == "pitch")
        c.pitchDeg = v;
    else if (path == "roll")
        c.rollDeg = v;
    else if (path == "yaw_rate")
        c.yawRateDeg = v;
    else if (path == "pitch_rate")
        c.pitchRateDeg = v;
    else if (path == "roll_rate")
        c.rollRateDeg = v;
    else if (path == "level")
        c.levelDb = static_cast<float>(v);
    else if (path == "rates")
        return flag(value, c.ratesRun);
    else if (path == "head.yaw")
        s.headYaw = v;
    else if (path == "head.pitch")
        s.headPitch = v;
    else if (path == "head.roll")
        s.headRoll = v;
    else
        return false;
    c.tracker = headFromYpr(s.headYaw, s.headPitch, s.headRoll);
    return true;
}

bool readHead(const std::string& path, HeadModel& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot read %s\n", path.c_str());
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string error;
    if (!parseHeadModel(text, out, &error)) {
        std::fprintf(stderr, "%s: %s\n", path.c_str(), error.c_str());
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    //  Denormals flushed, as the plugin processes.
    ScopedFlushDenormals flushDenormals;
    int order = 3, fieldOrder = -1, voices = 3, blockSize = 512, busChannels = 0;
    double seconds = 3.0, impulseAz = 0.0, impulseEl = 0.0;
    bool impulse = false;
    std::string outPath, tracePath, heads = std::string(BAMBI_CORE_DIR) + "/data/decode";
    Settings settings;
    std::vector<tools::Change> changes;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&](const char* flagName) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", flagName);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (a == "--order")
            order = std::atoi(next("--order").c_str());
        else if (a == "--field-order")
            fieldOrder = std::atoi(next("--field-order").c_str());
        else if (a == "--seconds")
            seconds = std::atof(next("--seconds").c_str());
        else if (a == "--voices")
            voices = std::atoi(next("--voices").c_str());
        else if (a == "--channels")
            busChannels = std::atoi(next("--channels").c_str());
        else if (a == "--block")
            blockSize = std::atoi(next("--block").c_str());
        else if (a == "--heads")
            heads = next("--heads");
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
            bool built = false;
            if (!tools::splitSetting(next("--set"), path, value) || !apply(settings, path, value, built)) {
                std::fprintf(stderr, "--set: no such setting, or not such a value\n");
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
    if (fieldOrder < 0) fieldOrder = order;
    if (outPath.empty() || order < 0 || order > 7 || fieldOrder > order || blockSize < 1 || seconds <= 0.0 ||
        busChannels < 0 || busChannels > kMaxDecoderOutputs) {
        usage();
        return 2;
    }

    const double fs = 48000.0;
    const int C = numChannels(order);
    const auto frames = static_cast<std::size_t>(seconds * fs);

    // ---- the input field, interleaved on the bus ------------------------------------------------
    std::vector<float> in(frames * static_cast<std::size_t>(C), 0.0f);
    std::vector<double> y(static_cast<std::size_t>(numChannels(fieldOrder)));
    const auto F = static_cast<std::size_t>(numChannels(fieldOrder));
    if (impulse) {
        shSN3D(fromAzEl(impulseAz * kDeg2Rad, impulseEl * kDeg2Rad), fieldOrder, y);
        const auto at = static_cast<std::size_t>(0.01 * fs);  // past the start, so a delay before it is seen
        for (std::size_t c = 0; c < F && at < frames; ++c)
            in[at * static_cast<std::size_t>(C) + c] = static_cast<float>(0.5 * y[c]);
    } else {
        unsigned seed = 0x2545F491u;
        for (int v = 0; v < std::max(1, voices); ++v) {
            const double az = 20.0 + 360.0 * v / std::max(1, voices), el = v % 2 == 0 ? 15.0 : -20.0;
            shSN3D(fromAzEl(az * kDeg2Rad, el * kDeg2Rad), fieldOrder, y);
            const double level = 0.1 / (1.0 + 0.5 * v);
            for (std::size_t f = 0; f < frames; ++f) {
                seed = seed * 1664525u + 1013904223u;
                const double sample = level * (static_cast<int>(seed >> 8) % 20001 - 10000) / 10000.0;
                for (std::size_t c = 0; c < F; ++c)
                    in[f * static_cast<std::size_t>(C) + c] += static_cast<float>(sample * y[c]);
            }
        }
    }

    // ---- the engine, built before the first block -------------------------------------------------
    HeadModel model, ku100;
    if (!readHead(heads + "/head.json", model) || !readHead(heads + "/ku100.json", ku100)) return 1;
    DecoderEngine engine;
    engine.setHeads(model, ku100);
    engine.prepare(settings.state, order, fs, blockSize, settings.controls.levelDb);
    if (!engine.designError().empty()) {
        std::fprintf(stderr, "%s\n", engine.designError().c_str());
        return 1;
    }
    //  The bus: as wide as asked, or as what plays needs.
    if (busChannels == 0) {
        const DecoderState& d = settings.state;
        busChannels = 2;
        if (d.headphonesPlay) busChannels = std::max(busChannels, d.headphoneChannel + 1);
        if (d.speakersPlay) busChannels = std::max(busChannels, d.speakerFirstChannel - 1 + engine.speakerOutputs());
    }

    AudioBuffer wav;
    wav.sampleRate = static_cast<int>(fs);
    wav.channels.assign(static_cast<std::size_t>(busChannels), std::vector<float>(frames, 0.0f));
    std::vector<float*> out(static_cast<std::size_t>(busChannels));

    std::FILE* trace = nullptr;
    if (!tracePath.empty()) {
        trace = std::fopen(tracePath.c_str(), "wb");
        if (trace == nullptr) {
            std::fprintf(stderr, "cannot write %s\n", tracePath.c_str());
            return 1;
        }
        std::fprintf(trace, "t,energy,headphones,speakers,head_yaw,head_pitch,scene_yaw,scene_pitch,outputs\n");
    }

    /*  Walked in segments bounded by the control grid and by the host's block, never by the block alone, so
     *  --block 1 and --block 1024 give the same samples. Checked, not assumed. */
    std::size_t pos = 0, stepStart = 0;
    tools::ChangeSchedule schedule(std::move(changes));
    const auto energyOf = [&](int from, int to) {
        double e = 0.0;
        for (int c = std::max(0, from); c < std::min(to, busChannels); ++c)
            for (std::size_t f = stepStart; f < pos; ++f) {
                const double s = wav.channels[static_cast<std::size_t>(c)][f];
                e += s * s;
            }
        return e;
    };
    const auto closeStep = [&] {
        if (trace == nullptr || pos <= stepStart) return;
        const DecoderState& d = settings.state;
        const int hp = d.headphoneChannel - 1, sp = std::max(1, d.speakerFirstChannel) - 1;
        double headYaw, headPitch, headRoll, sceneYaw, scenePitch, sceneRoll;
        yprOf(engine.head(), headYaw, headPitch, headRoll);
        yprOf(engine.scene(), sceneYaw, scenePitch, sceneRoll);
        std::fprintf(trace, "%.6f,%.9e,%.9e,%.9e,%.6f,%.6f,%.6f,%.6f,%d\n", static_cast<double>(stepStart) / fs,
                     energyOf(0, busChannels), engine.headphonesHeard() ? energyOf(hp, hp + 2) : 0.0,
                     engine.speakersHeard() ? energyOf(sp, sp + engine.speakerOutputs()) : 0.0, headYaw, headPitch,
                     sceneYaw, scenePitch, engine.speakerOutputs());
    };
    while (pos < frames) {
        if (pos % kStep == 0) {
            closeStep();
            stepStart = pos;
            bool rebuild = false;
            const tools::Change* refused = schedule.applyDue(fs, pos, [&](const tools::Change& c) {
                if (c.reset()) {
                    engine.restartRates();
                    return true;
                }
                bool built = false;
                const bool ok = apply(settings, c.path, c.value, built);
                rebuild = rebuild || built;
                return ok;
            });
            if (refused != nullptr) {
                std::fprintf(stderr, "--at: no such setting %s\n", refused->path.c_str());
                return 2;
            }
            if (rebuild) engine.build(settings.state);
        }
        const std::size_t toStep = kStep - pos % kStep,
                          toBlock = static_cast<std::size_t>(blockSize) - pos % static_cast<std::size_t>(blockSize);
        const auto n = static_cast<int>(std::min({toStep, toBlock, frames - pos}));
        for (int c = 0; c < busChannels; ++c)
            out[static_cast<std::size_t>(c)] = wav.channels[static_cast<std::size_t>(c)].data() + pos;
        engine.process(settings.state, settings.controls, in.data() + pos * static_cast<std::size_t>(C), out.data(),
                       busChannels, n);
        pos += static_cast<std::size_t>(n);
    }
    closeStep();
    if (trace != nullptr) std::fclose(trace);

    std::string err;
    if (!writeWav(outPath, wav, &err)) {
        std::fprintf(stderr, "%s: %s\n", outPath.c_str(), err.c_str());
        return 1;
    }
    std::printf("wrote    %d channels, %.2f s, %d loudspeaker outputs\n", busChannels, seconds,
                engine.speakerOutputs());
    return 0;
}
