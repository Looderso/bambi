// SPDX-License-Identifier: GPL-3.0-or-later
//
//  bambi-bench -- the standing performance record.
//
//  This exists because performance claims made from memory drift, and because a Debug build
//  can give an entirely wrong picture of where the cost is. Measure in Release, from here.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "bambi/dsp/features.hpp"
#include "bambi/dsp/fft.hpp"
#include "bambi/dsp/field.hpp"
#include "bambi/echo/engine.hpp"
#include "bambi/echo/pass.hpp"
#include "bambi/echo/timing.hpp"
#include "bambi/echo/warp.hpp"
#include "bambi/encode/encoder.hpp"
#include "bambi/encode/params.hpp"
#include "bambi/link/latest.hpp"
#include "bambi/link/link.hpp"
#include "bambi/math/sh.hpp"
#include "bambi/math/shrotation.hpp"
#include "bambi/mod/modulation.hpp"
#include "bambi/patch/json.hpp"
#include "bambi/path/generator.hpp"
#include "bambi/path/trajectory.hpp"
#include "bambi/region/projection.hpp"
#include "bambi/reverb/early.hpp"
#include "bambi/reverb/engine.hpp"
#include "bambi/reverb/tail.hpp"

using namespace bambi;
using Clock = std::chrono::steady_clock;

namespace {

//  Median of repeated batches, not mean: one scheduler preemption in a batch skews a mean
//  and cannot skew a median.
template <typename F>
double medianMicros(int batches, int itersPerBatch, F&& f) {
    std::vector<double> t;
    t.reserve(static_cast<std::size_t>(batches));
    for (int b = 0; b < batches; ++b) {
        const auto t0 = Clock::now();
        for (int i = 0; i < itersPerBatch; ++i) f();
        const auto t1 = Clock::now();
        t.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count() / itersPerBatch);
    }
    std::sort(t.begin(), t.end());
    return t[t.size() / 2];
}

void row(const char* name, double us, const char* note = "") { std::printf("  %-24s %8.3f us   %s\n", name, us, note); }

}  // namespace

int main() {
#ifndef NDEBUG
    std::printf("\n  *** DEBUG BUILD -- these numbers are meaningless. ***\n");
#endif
    std::printf("\nbambi-bench\n\n");

    //  A realistic block: 128 samples at 48 kHz is 2.667 ms of wall clock. Everything is
    //  reported against that, because "microseconds" alone never answered the real
    //  question, which is how many instances fit in a session.
    constexpr int kBlock = 128;
    constexpr double kBlockMs = kBlock / 48000.0 * 1000.0;

    std::vector<float> sig(4096);
    for (std::size_t i = 0; i < sig.size(); ++i) sig[i] = 0.5f * std::sin(0.07f * static_cast<float>(i));

    {
        RealFft fft;
        fft.prepare(2048);
        std::vector<double> in(2048), mag(fft.bins());
        for (std::size_t i = 0; i < in.size(); ++i) in[i] = sig[i];
        row("fft 2048", medianMicros(9, 2000, [&] { fft.magnitude(in, mag); }));
    }

    for (int order : {1, 3, 7}) {
        std::vector<double> y(static_cast<std::size_t>(numChannels(order)));
        double a = 0.0;
        const double us = medianMicros(9, 20000, [&] {
            a += 0.001;
            shSN3D(fromAzEl(a, 0.3), order, y);
        });
        row(("shSN3D order " + std::to_string(order)).c_str(), us);
    }

    for (int order : {1, 3, 7}) {
        const int nch = numChannels(order);
        Encoder enc;
        enc.prepare(order);
        std::vector<std::vector<float>> bufs(static_cast<std::size_t>(nch), std::vector<float>(kBlock, 0.0f));
        std::vector<float*> ptrs;
        for (auto& b : bufs) ptrs.push_back(b.data());

        double a = 0.0;
        const double us = medianMicros(9, 20000, [&] {
            a += 0.01;
            enc.setTarget(fromAzEl(a, 0.2), 0.8);
            enc.process(sig.data(), ptrs, kBlock);
        });
        char note[64];
        std::snprintf(note, sizeof note, "%2dch  %.3f%% of a core", nch, us / (kBlockMs * 1000.0) * 100.0);
        row(("encode block " + std::to_string(order)).c_str(), us, note);
    }

    {
        /*  prepare()'s second argument is fftSize, not the block size -- passing kBlock
         *  here would silently benchmark a 128-point FFT and report a number several times
         *  better than the truth. The defaults (2048 / hop 256) are the shipping
         *  configuration and the only ones worth measuring. */
        FeatureBank fb;
        fb.prepare(48000.0);
        const double us = medianMicros(9, 5000, [&] { fb.process(sig.data(), kBlock); });
        char note[80];
        std::snprintf(note, sizeof note, "%.3f%% of a core, spectral on", us / (kBlockMs * 1000.0) * 100.0);
        row("features block", us, note);

        FeatureBank fbNo;
        fbNo.prepare(48000.0);
        fbNo.setSpectralEnabled(false);
        const double us2 = medianMicros(9, 5000, [&] { fbNo.process(sig.data(), kBlock); });
        char note2[80];
        std::snprintf(note2, sizeof note2, "%.3f%% of a core, spectral off", us2 / (kBlockMs * 1000.0) * 100.0);
        row("features block", us2, note2);

        /*  A field's detector: the power of every channel, a block, and the bank fed it beside
         *  W. Planar, as a host hands a field over. */
        for (const int order : {1, 3, 7}) {
            const int channels = (order + 1) * (order + 1);
            std::vector<std::vector<float>> field(static_cast<std::size_t>(channels), sig);
            std::vector<const float*> pointers;
            for (const auto& c : field) pointers.push_back(c.data());
            std::vector<float> power(static_cast<std::size_t>(kBlock));
            const double usP = medianMicros(9, 5000, [&] { fieldPower(pointers, kBlock, power.data()); });
            char noteP[80];
            std::snprintf(noteP, sizeof noteP, "%.4f%% of a core, order %d", usP / (kBlockMs * 1000.0) * 100.0, order);
            row("field power block", usP, noteP);
        }
        FeatureBank fbField;
        fbField.prepare(48000.0);
        std::vector<float> power(sig.size(), 0.01f);
        const double us3 = medianMicros(9, 5000, [&] { fbField.process(sig.data(), power.data(), kBlock); });
        char note3[80];
        std::snprintf(note3, sizeof note3, "%.3f%% of a core, spectral on, power given",
                      us3 / (kBlockMs * 1000.0) * 100.0);
        row("features block", us3, note3);
    }

    {
        //  The modulation engine runs once per control hop, not once per block, but it is
        //  reported per block like everything else so the column adds up to an instance.
        PluginState st{bambi::encodeParams()};
        for (int i = 0; i < 8; ++i)
            st.matrix.push_back(MatrixCell{MatrixTab::Features, i % 6,
                                           i % 2 ? EncoderParam::MotionSpeed : EncoderParam::RenderWidth, 0.5});
        ModulationEngine mod;
        mod.useManifest(encodeMod());
        mod.prepare(48000.0);
        mod.setState(st);
        MotionClock clock;
        Transport tp;
        const std::vector<double> f{0.5, 0.3, 0.7, 0.2, 0.4, 0.1};
        const double us = medianMicros(9, 20000, [&] {
            mod.process(f, {}, tp, 1.0 / 187.5);
            clock.advance(mod.destination(EncoderParam::MotionSpeed), 6.28, 1.0 / 187.5);
        });
        char note[64];
        std::snprintf(note, sizeof note, "8 cells, per control hop");
        row("modulation step", us, note);
    }

    {
        //  The audio thread's entire contribution to the link bus is `link submit`: a copy
        //  into process-local memory and one atomic exchange. Everything after it runs on
        //  the message or UI thread.
        LatestValue<LinkDynamic> latest;
        LinkDynamic dyn;
        row("link submit", medianMicros(9, 50000, [&] { latest.write(dyn); }), "audio thread");

        const Uuid session = Uuid::generate();
        LinkBus bus;
        if (bus.open(session, Uuid::generate(), Product::Encoder)) {
            row("link publish dynamic", medianMicros(9, 50000, [&] { bus.publishDynamic(dyn); }),
                "message thread, per frame");

            TrajectoryState ts;
            ts.kind = TrajectoryKind::Parametric;
            ts.generator = GeneratorType::Lissajous;
            generatorDefaults(ts.generator, ts.genParams);
            Trajectory path;
            path.build(ts);
            LinkStatic st;
            st.setPath(path);
            row("link publish static", medianMicros(9, 20000, [&] { bus.publishStatic(st); }),
                "message thread, on edit");

            //  The steady state of a session: nine instances, nobody editing.
            std::vector<std::unique_ptr<LinkBus>> peers;
            for (int i = 0; i < 8; ++i) {
                auto b = std::make_unique<LinkBus>();
                if (b->open(session, Uuid::generate(), Product::Encoder)) {
                    b->publishStatic(st);
                    peers.push_back(std::move(b));
                }
            }
            LinkScene scene;
            scene.update(bus);  // the first update copies every path once
            row("link scene update", medianMicros(9, 5000, [&] { scene.update(bus); }), "UI thread, 9 instances");
            peers.clear();
            bus.close();
            LinkBus::unlinkSession(session);
        }

        Json o = Json::object();
        for (int i = 0; i < 60; ++i) o.set("param." + std::to_string(i), Json{i * 0.5});
        const std::string text = o.dump(2);
        row("json dump 60 keys", medianMicros(9, 2000, [&] { (void)o.dump(2); }));
        row("json parse 60 keys", medianMicros(9, 2000, [&] { (void)Json::parse(text); }));
    }

    /*  Turning a field. Build is once per control step if the axis or the angle moves -- 187.5 times
     *  a second -- and apply is per sample. Both are reported per call; the note gives the share of one
     *  core that implies. If build did not rise steeply with the order, or apply did not track the sum
     *  of (2n+1)^2, the loop was optimised away and the row measured nothing. */
    std::printf("\n  field rotation (math/shrotation)\n");
    for (const int order : {1, 3, 5, 7}) {
        std::vector<double> blocks(static_cast<std::size_t>(rotationSize(order)));
        double angle = 0.1;
        double sink = 0.0;
        const double buildUs = medianMicros(9, 2000, [&] {
            angle += 1e-4;
            shRotation(rotationAbout({0.3, -1.0, 0.6}, angle), order, blocks);
            sink += blocks.back();
        });
        std::vector<double> in(static_cast<std::size_t>(numChannels(order)), 0.25), out(in.size());
        const double applyUs = medianMicros(9, 20000, [&] {
            in[0] += 1e-9;
            rotateSH(blocks, order, in, out);
            sink += out.back();
        });
        char name[48], note[96];
        std::snprintf(name, sizeof name, "rotation build, order %d", order);
        std::snprintf(note, sizeof note, "%.4f %% of a core at one a control step", buildUs * 187.5 / 1e4);
        row(name, buildUs, note);
        std::snprintf(name, sizeof name, "rotation apply, order %d", order);
        std::snprintf(note, sizeof note, "%.3f %% of a core per sample, double, one frame at a time",
                      applyUs * 48000.0 / 1e4);
        row(name, applyUs, note);
        if (sink != sink) std::printf("  (nan)\n");
    }

    /*  The float field kernels, a 128-frame block at a time, per frame. The rotation is the one to
     *  watch: fixed-size it should be several times faster than the double, frame-at-a-time apply above,
     *  and the run-time-width order (9) should not be -- if it is as fast, the fixed kernels bought
     *  nothing; if the checksum is zero, nothing was computed. */
    std::printf("\n  field kernels, float, interleaved (dsp/field)\n");
    for (const int order : {1, 3, 5, 7, 9}) {
        const int ch = numChannels(order);
        std::vector<double> blocks(static_cast<std::size_t>(rotationSize(order)));
        shRotation(rotationAbout({0.3, -1.0, 0.6}, 0.9), order, blocks);
        std::vector<float> fwd(blocks.size()), inv(blocks.size());
        fieldBlocks(blocks, order, fwd, inv);
        std::vector<float> in(static_cast<std::size_t>(ch * kBlock)), out(in.size());
        for (std::size_t k = 0; k < in.size(); ++k) in[k] = 0.001f * static_cast<float>(k % 97);
        std::vector<float> c(static_cast<std::size_t>(order + 1), 0.8f), sn(c.size(), 0.6f), g(c.size(), 0.99f);
        double sink = 0.0;
        const double turnUs = medianMicros(9, 400,
                                           [&] {
                                               in[0] += 1e-6f;
                                               rotateField(fwd, order, in.data(), out.data(), kBlock);
                                               sink += out[out.size() - 1];
                                           }) /
                              kBlock;
        const double spinUs = medianMicros(9, 400,
                                           [&] {
                                               spinField(c, sn, order, out.data(), kBlock);
                                               orderGains(g, order, out.data(), kBlock);
                                               sink += out[1];
                                           }) /
                              kBlock;
        char name[48], note[96];
        std::snprintf(name, sizeof name, "turn a field, order %d", order);
        std::snprintf(note, sizeof note, "%.3f %% of a core per turn%s", turnUs * 48000.0 / 1e4,
                      order > kFixedOrder ? "   (run-time widths)" : "");
        row(name, turnUs, note);
        std::snprintf(name, sizeof name, "spin + gains, order %d", order);
        std::snprintf(note, sizeof note, "%.3f %% of a core", spinUs * 48000.0 / 1e4);
        row(name, spinUs, note);
        if (sink == 0.0) std::printf("  (checksum zero: nothing was computed)\n");
    }

    /*  Echo's skew, built: once a control step for a tap whose skew is modulated, never otherwise. If
     *  a repeated skew cost less than a changing one something is cached and the row is not the cost
     *  of a rebuild; the skew changes every call so that it cannot be. */
    std::printf("\n  echo: the skew's matrix, built (echo/warp)\n");
    for (const int order : {1, 3, 5, 7}) {
        WarpBuilder builder;
        builder.prepare(order);
        std::vector<double> blocks(static_cast<std::size_t>(warpSize(order)));
        double skew = -0.6, sink = 0.0;
        const double us = medianMicros(9, 500, [&] {
            skew = skew > 0.6 ? -0.6 : skew + 1e-3;
            builder.build(skew, blocks);
            sink += blocks.back();
        });
        char name[48], note[96];
        std::snprintf(name, sizeof name, "skew build, order %d", order);
        std::snprintf(note, sizeof note, "%.3f %% of a core a tap, if rebuilt every control step", us * 187.5 / 1e4);
        row(name, us, note);
        if (sink == 0.0) std::printf("  (checksum zero: nothing was computed)\n");
    }

    /*  Echo's pass, the whole chain, four taps on a 128-frame block -- the number the engine's cost
     *  rests on, without the rings. "tilted" turns to the axis and back; "pole" skips both turns and
     *  must be cheaper by about two turns a tap, or a turn is being skipped where it should not be. A
     *  zero checksum means nothing ran. */
    std::printf("\n  echo: one pass, four taps, a 128-frame block (echo/pass)\n");
    for (const int order : {1, 3, 5, 7}) {
        for (const bool pole : {false, true}) {
            PassWork work;
            work.prepare(order, kBlock);
            std::array<TapTransform, 4> taps;
            std::array<BandState, 4> bands;
            for (int k = 0; k < 4; ++k) {
                prepareTap(taps[static_cast<std::size_t>(k)], order);
                bands[static_cast<std::size_t>(k)].prepare(order);
                TapSettings set;
                set.axis = unit(Vec3{0.3 + 0.2 * k, -0.6, 0.5});
                set.axisIsPole = pole;
                if (pole) set.axis = {0, 0, 1};
                set.skew = 0.2 + 0.1 * k;
                set.spinRad = 0.5 * (k + 1);
                set.blurRad = 6.0 * kDeg2Rad;
                set.lowCutHz = 80.0;
                set.highCutHz = 8000.0;
                buildTap(set, 48000.0, work, taps[static_cast<std::size_t>(k)]);
            }
            std::vector<float> field(static_cast<std::size_t>(numChannels(order) * kBlock)), source(field.size());
            for (std::size_t k = 0; k < source.size(); ++k) source[k] = 0.001f * static_cast<float>((k * 31) % 101);
            double sink = 0.0;
            const double us = medianMicros(9, 200, [&] {
                std::memcpy(field.data(), source.data(), field.size() * sizeof(float));
                for (std::size_t k = 0; k < 4; ++k) onePass(taps[k], bands[k], work, field.data(), kBlock);
                sink += field[field.size() / 2];
            });
            char name[48], note[96];
            std::snprintf(name, sizeof name, "4 taps, order %d, %s", order, pole ? "pole" : "tilted");
            std::snprintf(note, sizeof note, "%.2f %% of a core", 100.0 * us / (kBlockMs * 1000.0));
            row(name, us, note);
            if (sink == 0.0) std::printf("  (checksum zero: nothing was computed)\n");
        }
    }

    /*  Echo's engine: four awake, tilted, skewed taps with rings of 0.75 to the cap, fed noise, a frame
     *  every other block as a host would give it. Against the pass alone above, what is added is the
     *  rings, the input line and the guards. If the checksum is zero the taps slept; if short rings
     *  and long ones differed it would be timing page faults, which prepare() exists to prevent. */
    std::printf("\n  echo: the engine, four taps, a 128-frame block (echo/engine)\n");
    for (const int order : {1, 3, 5, 7}) {
        EchoEngine engine;
        engine.prepare(order, 48000.0);
        EchoFrame frame;
        const double cap = maxTapSeconds(order);
        for (int k = 0; k < kEchoTaps; ++k) {
            EchoTapFrame& t = frame.taps[static_cast<std::size_t>(k)];
            t.on = true;
            t.periodSamples = static_cast<int>((0.75 + (cap - 0.75) * k / 3.0) * 48000.0);
            t.offsetSamples = 1000 * k;
            t.level = 0.7;
            t.feedback = 0.8;
            t.pass.axis = unit(Vec3{0.3 + 0.2 * k, -0.6, 0.5});
            t.pass.axisIsPole = false;
            t.pass.skew = 0.2 + 0.1 * k;
            t.pass.spinRad = 0.5 * (k + 1);
            t.pass.blurRad = 6.0 * kDeg2Rad;
            t.pass.lowCutHz = 80.0;
            t.pass.highCutHz = 8000.0;
        }
        engine.setFrame(frame);
        const int ch = numChannels(order);
        std::vector<float> in(static_cast<std::size_t>(ch * kBlock)), out(in.size());
        unsigned seed = 1u;
        for (float& v : in) {
            seed = seed * 1664525u + 1013904223u;
            v = static_cast<float>(static_cast<int>(seed >> 8) % 2001 - 1000) / 4000.0f;
        }
        for (int warm = 0; warm < 1200; ++warm) engine.process(in.data(), out.data(), kBlock);  // past the longest tap
        double sink = 0.0;
        int block = 0;
        const double us = medianMicros(9, 300, [&] {
            if ((++block & 1) == 0) engine.setFrame(frame);
            engine.process(in.data(), out.data(), kBlock);
            sink += out[out.size() / 3];
        });
        char name[48], note[96];
        std::snprintf(name, sizeof name, "echo engine, order %d", order);
        std::snprintf(note, sizeof note, "%.2f %% of a core, %.0f MB of rings", 100.0 * us / (kBlockMs * 1000.0),
                      5.0 * engine.capacityFrames() * ch * 4.0 / 1.0e6);
        row(name, us, note);
        if (sink == 0.0) std::printf("  (checksum zero: the taps slept)\n");
    }

    /*  A region on the bus, axial kinds. Set is once a control step for a region that is moving, never
     *  otherwise; apply is a 128-frame block. If set cost the same for a spot and for everywhere, the
     *  integral is not running; a zero checksum means the field never went through it. */
    std::printf("\n  region on the bus (region/projection)\n");
    for (const int order : {1, 3, 5, 7}) {
        RegionOperator op;
        op.prepare(order, kBlock);
        Region r;
        r.kind = RegionKind::Spot;
        r.size = 60.0 * kDeg2Rad;
        r.softness = 40.0 * kDeg2Rad;
        r.pitch = 0.3;
        double sink = 0.0;
        const double setUs = medianMicros(9, 300, [&] {
            r.yaw += 1e-3;
            r.size += 1e-5;
            sink += op.set(r) ? op.blocks()[1] : 0.0;
        });
        std::vector<float> in(static_cast<std::size_t>(numChannels(order) * kBlock), 0.25f), out(in.size());
        const double applyUs = medianMicros(9, 300, [&] {
            in[0] += 1e-6f;
            op.apply(in.data(), out.data(), kBlock);
            sink += out[out.size() / 2];
        });
        char name[48], note[96];
        std::snprintf(name, sizeof name, "region set, order %d", order);
        std::snprintf(note, sizeof note, "%.3f %% of a core, if it moves every control step", setUs * 187.5 / 1e4);
        row(name, setUs, note);
        std::snprintf(name, sizeof name, "region apply, order %d", order);
        std::snprintf(note, sizeof note, "%.2f %% of a core", 100.0 * applyUs / (kBlockMs * 1000.0));
        row(name, applyUs, note);
        if (sink == 0.0) std::printf("  (checksum zero)\n");
    }

    /*  A dense region whose shape moves every control step -- a modulated fill, dot size or cloud.
     *  Only a step whose shape changed pays this; a turn pays the rotation above. If dots cost the same at
     *  every order the harmonics are not being built; a zero checksum means the matrix stayed empty. */
    std::printf("\n  a dense region's shape, rebuilt in the step (region/projection)\n");
    for (const int order : {1, 3, 5, 7, 10}) {
        for (const RegionKind kind : {RegionKind::Sectors, RegionKind::Dots, RegionKind::Clouds}) {
            RegionOperator op;
            op.prepare(order, kBlock);
            Region r;
            r.kind = kind;
            r.sectors = 4, r.dots = 20, r.seed = 7;
            r.fill = 0.5, r.dotSize = 0.2, r.softness = 0.3;
            double sink = 0.0;
            const double us = medianMicros(9, 100, [&] {
                r.fill += 1e-5, r.dotSize += 1e-6, r.softness += 1e-6, r.evolve += 1e-4;
                op.set(r);
                sink += op.dense()[0];
            });
            char name[48], note[96];
            std::snprintf(name, sizeof name, "%s shape, order %d",
                          kind == RegionKind::Sectors ? "sectors"
                          : kind == RegionKind::Dots  ? "20 dots"
                                                      : "clouds",
                          order);
            std::snprintf(note, sizeof note, "%.3f %% of a core, if it moves every control step", us * 187.5 / 1e4);
            row(name, us, note);
            if (sink == 0.0) std::printf("  (checksum zero)\n");
        }
    }

    /*  Reverb's tail, a 128-frame block, fed noise. It knows nothing of ambisonics, so its cost is the
     *  line count's and not the bus order's. If 8, 16 and 32 lines did not cost roughly in proportion the
     *  loop is not what is being timed; a zero checksum means nothing came out. */
    std::printf("\n  reverb: the tail, a 128-frame block (reverb/tail)\n");
    for (const int lines : {8, 16, 32}) {
        FdnTail tail;
        tail.prepare(48000.0);
        tail.configure(planTail(lines, 48000.0, 1.2, 0.06));
        tail.set(TailSettings{});
        std::vector<float> in(static_cast<std::size_t>(lines * kBlock)), out(in.size());
        unsigned seed = 3u;
        for (float& v : in) {
            seed = seed * 1664525u + 1013904223u;
            v = static_cast<float>(static_cast<int>(seed >> 8) % 2001 - 1000) / 8000.0f;
        }
        for (int warm = 0; warm < 400; ++warm) tail.process(in.data(), nullptr, out.data(), kBlock);
        double sink = 0.0;
        const double us = medianMicros(9, 300, [&] {
            tail.process(in.data(), nullptr, out.data(), kBlock);
            sink += out[out.size() / 2];
        });
        char name[48], note[96];
        std::snprintf(name, sizeof name, "reverb tail, %d lines", lines);
        std::snprintf(note, sizeof note, "%.2f %% of a core", 100.0 * us / (kBlockMs * 1000.0));
        row(name, us, note);
        if (sink == 0.0) std::printf("  (checksum zero)\n");
    }

    /*  Reverb's early reflections on an order-3 bus, a 128-frame block, per preset. The cost goes with
     *  the live taps, which the room decides: a small room has many more reflections inside its mixing
     *  time than a cathedral. If cost did not follow the tap count the inner loop is not what is timed. */
    std::printf("\n  reverb: early reflections on an order-3 bus, a 128-frame block (reverb/early)\n");
    {
        struct Preset {
            const char* name;
            RoomSettings room;
            double distance;
        };
        const Preset presets[] = {{"ambience", {4.0, RoomShape::Room, 0.35, 0.50, 0.80}, 1.5},
                                  {"room", {5.0, RoomShape::Room, 0.5, 0.42, 0.75}, 2.0},
                                  {"chamber", {7.0, RoomShape::Tall, 1.2, 0.55, 0.70}, 3.0},
                                  {"hall", {20.0, RoomShape::Hall, 1.9, 0.50, 0.60}, 6.0},
                                  {"large hall", {28.0, RoomShape::Hall, 2.8, 0.45, 0.60}, 10.0},
                                  {"cathedral", {38.0, RoomShape::Tall, 6.5, 0.35, 0.50}, 14.0}};
        EarlyReflections early;
        early.prepare(3, 48000.0);
        std::vector<float> in(static_cast<std::size_t>(16 * kBlock)), field(in.size()),
            scattered(static_cast<std::size_t>(kVirtualSources * kBlock));
        unsigned seed = 9u;
        for (float& v : in) {
            seed = seed * 1664525u + 1013904223u;
            v = static_cast<float>(static_cast<int>(seed >> 8) % 2001 - 1000) / 4000.0f;
        }
        for (const Preset& p : presets) {
            const Room room = deriveRoom(p.room);
            double sink = 0.0;
            const double setUs = medianMicros(5, 20, [&] {
                early.setRoom(room, p.distance);
                sink += early.level();
            });
            for (int warm = 0; warm < 400; ++warm) early.process(in.data(), field.data(), scattered.data(), kBlock);
            const double us = medianMicros(9, 200, [&] {
                early.process(in.data(), field.data(), scattered.data(), kBlock);
                sink += field[field.size() / 2];
            });
            char name[48], note[120];
            std::snprintf(name, sizeof name, "%s", p.name);
            std::snprintf(note, sizeof note, "%.2f %% of a core, %d live taps; a room change costs %.0f us",
                          100.0 * us / (kBlockMs * 1000.0), early.liveTaps(), setUs);
            row(name, us, note);
            if (sink == 0.0) std::printf("  (checksum zero)\n");
        }
    }

    /*  Reverb's engine, whole: reflections, the bus read at the lines, the tail, the lines placed back.
     *  Per preset on an order-3 bus, and the hall on orders 1 to 7 -- the tail and the reflections run at
     *  order 3 whatever the bus, so a higher order should cost little more. */
    std::printf("\n  reverb: the engine, a 128-frame block (reverb/engine)\n");
    {
        struct Case {
            const char* name;
            int order;
            RoomSettings room;
            double distance;
            int lines;
            bool send, ret, grow;
            int tailOrder;
        };
        const RoomSettings hallRoom{20.0, RoomShape::Hall, 1.9, 0.50, 0.60};
        Region aimed;
        aimed.kind = RegionKind::Spot;
        aimed.size = 50.0 * kDeg2Rad;
        aimed.softness = 10.0 * kDeg2Rad;
        const Case cases[] = {
            {"ambience, order 3", 3, {4.0, RoomShape::Room, 0.35, 0.50, 0.80}, 1.5, 16, false, false, false, 3},
            {"hall, order 3", 3, {20.0, RoomShape::Hall, 1.9, 0.50, 0.60}, 6.0, 16, false, false, false, 3},
            {"cathedral, order 3", 3, {38.0, RoomShape::Tall, 6.5, 0.35, 0.50}, 14.0, 16, false, false, false, 3},
            {"hall, order 1", 1, {20.0, RoomShape::Hall, 1.9, 0.50, 0.60}, 6.0, 16, false, false, false, 3},
            {"hall, order 7", 7, {20.0, RoomShape::Hall, 1.9, 0.50, 0.60}, 6.0, 16, false, false, false, 3},
            {"hall, order 3, 8 lines", 3, {20.0, RoomShape::Hall, 1.9, 0.50, 0.60}, 6.0, 8, false, false, false, 3},
            //  the send is gains at twelve directions and at the lines; the return adds
            //  the shared projection over the whole bus, which is where the order tells
            {"hall, order 3, a send", 3, hallRoom, 6.0, 16, true, false, false, 3},
            {"hall, order 3, a send and a return", 3, hallRoom, 6.0, 16, true, true, false, 3},
            {"hall, order 7, a send and a return", 7, hallRoom, 6.0, 16, true, true, false, 3},
            //  a size knob being dragged: two networks running while one fades past the other
            {"hall, order 3, a size being dragged", 3, hallRoom, 6.0, 16, false, false, true, 3},
            //  the levers, against the hall's baseline: the tail's order, its
            //  line count, and the two together
            {"hall, order 3, tail at order 1", 3, hallRoom, 6.0, 16, false, false, false, 1},
            {"hall, order 3, tail at order 0", 3, hallRoom, 6.0, 16, false, false, false, 0},
            {"hall, order 3, 8 lines at order 1", 3, hallRoom, 6.0, 8, false, false, false, 1}};
        for (const Case& c : cases) {
            ReverbEngine engine;
            engine.prepare(c.order, 48000.0);
            ReverbFrame s;
            s.room = c.room;
            s.distance = c.distance;
            s.tailLines = c.lines;
            s.tailOrder = c.tailOrder;
            if (c.send) s.send = aimed;
            if (c.ret) s.returnRegion = aimed;
            engine.set(s);
            const int ch = numChannels(c.order);
            std::vector<float> in(static_cast<std::size_t>(ch * kBlock)), out(in.size());
            unsigned seed = 11u;
            for (float& v : in) {
                seed = seed * 1664525u + 1013904223u;
                v = static_cast<float>(static_cast<int>(seed >> 8) % 2001 - 1000) / 8000.0f;
            }
            for (int warm = 0; warm < 400; ++warm) engine.process(in.data(), out.data(), kBlock);
            double sink = 0.0;
            int block = 0;
            const double us = medianMicros(9, 200, [&] {
                if ((++block & 1) == 0) {
                    if (c.grow) s.room.size = 20.0 + 6.0 * ((block >> 1) & 1);  // never settles: a fade always runs
                    engine.set(s);
                }
                engine.process(in.data(), out.data(), kBlock);
                sink += out[out.size() / 2];
            });
            char note[96];
            std::snprintf(note, sizeof note, "%.2f %% of a core", 100.0 * us / (kBlockMs * 1000.0));
            row(c.name, us, note);
            if (sink == 0.0) std::printf("  (checksum zero)\n");
        }
    }

    std::printf("\n  block = %d samples @ 48 kHz = %.3f ms\n\n", kBlock, kBlockMs);
    return 0;
}
