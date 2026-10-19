// SPDX-License-Identifier: GPL-3.0-or-later
#include <atomic>
#include <cstdlib>
#include <functional>
#include <new>

#include "../doctest.h"
#include "bambi/decode/binaural.hpp"
#include "bambi/decode/decoder.hpp"
#include "bambi/decode/headmodel.hpp"
#include "bambi/decode/headrotation.hpp"
#include "bambi/decode/speakers.hpp"
#include "bambi/dsp/features.hpp"
#include "bambi/echo/control.hpp"
#include "bambi/echo/params.hpp"
#include "bambi/encode/control.hpp"
#include "bambi/encode/params.hpp"
#include "bambi/math/sh.hpp"
#include "bambi/patch/state.hpp"
#include "bambi/path/generator.hpp"
#include "bambi/region/projection.hpp"
#include "bambi/reverb/control.hpp"
#include "bambi/reverb/params.hpp"
#include "bambi/scene/energy.hpp"

/*  Nothing on the audio thread allocates. Said about a control step, which is the one place in the
 *  suite that reads parameters by key: a key is built from a prefix and a field name, and building
 *  it with a std::string allocates once it passes 22 characters, which no key reaches today but a
 *  longer one would. Nothing catches that unless something counts allocations directly.
 *
 *  These counters replace the global operator new for the whole test binary, so the count is only
 *  read inside the narrow scopes below; everything else allocates freely, as it should.
 */
namespace {
std::atomic<long> gAllocations{0};
}

void* operator new(std::size_t n) {
    gAllocations.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(n == 0 ? 1 : n);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace bambi {
namespace {

long allocationsDuring(const std::function<void()>& work) {
    const long before = gAllocations.load();
    work();
    return gAllocations.load() - before;
}

}  // namespace

TEST_CASE("a control step allocates nothing (rule 9)") {
    //  Catches: a control step that allocates: scratch sized in the step, or a key built as a std::string
    //  past 22 characters.
    SUBCASE("Echo") {
        const ParamManifest& m = echoParams();
        PluginState st{m};
        ModulationEngine mod;
        mod.useManifest(echoMod());
        mod.prepare(48000.0);
        mod.setState(st);  // allocates: not a control step
        EchoResolver r;
        std::array<double, kNumFeatures> self{};
        EchoControlInput in;
        in.self = self;
        in.dt = static_cast<double>(kEchoHop) / 48000.0;
        in.order = 3;
        in.transport.bpm = 120.0;
        in.sendShape = {RegionKind::Spot, 4, 6};
        const std::array<float, kMaxParams> base = st.params;

        r.step(mod, m, base, in, 48000.0);  // the first step may warm something up
        CHECK(allocationsDuring([&] {
                  for (int i = 0; i < 64; ++i) r.step(mod, m, base, in, 48000.0);
              }) == 0);
    }

    SUBCASE("Reverb") {
        const ParamManifest& m = reverbParams();
        PluginState st{m};
        const RegionShape spot{RegionKind::Spot, 4, 6}, band{RegionKind::Band, 4, 6};
        ReverbResolver r;
        const auto once = [&] {
            r.resolve(reverbSettingsFrom(m, st.params, RoomShape::Hall, spot, band, RenderQuality::Realistic), false,
                      48000.0);
        };
        once();
        CHECK(allocationsDuring([&] {
                  for (int i = 0; i < 64; ++i) once();
              }) == 0);
    }
}

TEST_CASE("a path whose settings move every step is rebuilt without allocating") {
    //  Catches: the rebuild's scratch sized inside the step, and a modulated path allocates at every step.
    const ParamManifest& m = encodeParams();
    PluginState st{m};
    st.trajectory.generator = GeneratorType::Lissajous;
    generatorDefaults(GeneratorType::Lissajous, st.trajectory.genParams);
    st.trajectory.genParams[2] = 7, st.trajectory.genParams[3] = 7;  // the densest path there is
    st.params[static_cast<std::size_t>(EncoderParam::Lfo1Sync)] = 0.0f;
    st.params[static_cast<std::size_t>(EncoderParam::Lfo1Rate)] = 2.0f;
    st.matrix.push_back({MatrixTab::Generators, 0, EncoderParam::LissajousAzAmount, 0.3});
    ModulationEngine mod;
    mod.useManifest(encodeMod());
    mod.prepare(48000.0);
    mod.setState(st);
    EncoderControl control;
    Trajectory path;
    path.build(st.trajectory);
    std::array<double, kNumFeatures> self{};
    ControlInput in;
    in.self = self;
    in.dt = static_cast<double>(kControlHop) / 48000.0;
    in.transport.playing = true;
    in.path = &path;
    in.shape = &st.trajectory;
    const std::array<float, kMaxParams> base = st.params;
    control.step(mod, base, in);
    CHECK(allocationsDuring([&] {
              for (int i = 0; i < 16; ++i) control.step(mod, base, in);
          }) == 0);
}

TEST_CASE("a region whose shape moves every step is rebuilt without allocating") {
    //  Catches: a vector sized inside the build, and every step of a modulated dots region allocates.
    RegionOperator op;
    op.prepare(10, 64);  // the most an effect is prepared at
    Region r;
    r.seed = 5;
    for (const RegionKind kind : {RegionKind::Sectors, RegionKind::Dots, RegionKind::Clouds, RegionKind::Custom}) {
        r.kind = kind;
        r.dots = 20;
        op.set(r);
        CHECK(allocationsDuring([&] {
                  for (int i = 0; i < 16; ++i) {
                      r.fill = 0.3 + 0.01 * i, r.dotSize = 0.1 + 0.005 * i, r.softness = 0.2 + 0.01 * i;
                      r.evolve = 0.1 * i, r.weights[3] = 0.01 * i;
                      op.set(r);
                  }
              }) == 0);
    }
}

TEST_CASE("the energy visualiser's audio-thread half allocates nothing (rule 9)") {
    /*  The accumulate runs on every block. Catches: size the sums inside `add`, or take the window
        into a fresh vector, and this counts it. */
    constexpr int order = 3;
    CovarianceWindow window;
    window.prepare(order, 2400);  // 50 ms at 48 kHz: allocates, here, and never again

    std::vector<float> frames(static_cast<std::size_t>(numChannels(order)) * 128, 0.25f);
    std::vector<float> out(static_cast<std::size_t>(covarianceSize(order)), 0.0f);

    window.add(frames.data(), 128);  // the first call may warm something up
    CHECK(allocationsDuring([&] {
              for (int i = 0; i < 64; ++i) {
                  window.add(frames.data(), 128);
                  if (window.ready()) window.take(out);
              }
          }) == 0);
}

TEST_CASE("the head synthesiser allocates nothing once prepared") {
    //  Catches: a per-call vector for the shifted notch centres, or a result built into a fresh buffer.
    //  (Not an audio-thread call -- a worker rebuilds the filters -- but it runs on every slider move.)
    HeadModel m;
    m.axis = {10.0, 0.5, 40};
    m.smooth.order = 1;
    m.smooth.coef.assign(4 * 40, -3.0);
    m.ild.order = 2;
    m.ild.coef.assign(9 * 40, 0.5);
    m.ildWithSize.order = 2;
    m.ildWithSize.coef.assign(9 * 40, 0.1);
    m.itd.order = 1;
    m.itd.coef = {0.0, 400.0, 0.0, 0.0};
    m.notches = {2, 2.5, 0.25, 0.55, 20.0, 29.0, 0.1, 1.5, {}};
    m.notches.list.push_back({{24.0, 0, 0, 0, 0, 0, 0, 0, 0},
                              {-6.0, 0, 0, 0, 0, 0, 0, 0, 0},
                              {0.4, 0, 0, 0, 0, 0, 0, 0, 0},
                              {1.0, 0, 0, 0, 0, 0, 0, 0, 0}});
    REQUIRE(m.consistent());
    const std::array<Vec3, 3> dirs{Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}};
    Head head;
    head.prepare(m, dirs);
    head.synthesise(0.0, 1.0);
    CHECK(allocationsDuring([&] {
              head.synthesise(0.7, 1.2);
              head.synthesise(-1.0, 0.8);
          }) == 0);
    CHECK(head.itd(1) == doctest::Approx(0.8 * 400.0));

    //  Catches: a per-build buffer in the filter design, or an FFT prepared on the fly.
    std::vector<Vec3> grid;
    std::vector<double> weights;
    quadratureGrid(3, 5, grid, weights);
    FilterDesign design;
    design.order = 1;
    design.fftSize = 64;
    design.taps = 32;
    design.delay = 8;
    design.fade = 4;
    BinauralFilterBuilder builder;
    builder.prepare(m, design, grid, weights);
    builder.build(0.0, 1.0);
    CHECK(allocationsDuring([&] { builder.build(0.5, 1.1); }) == 0);

    //  Catches: a convolution that gathers its block into a fresh buffer, or a publish that transforms into one.
    MatrixConvolver convolver;
    REQUIRE(convolver.prepare(1, 32, 32));
    std::vector<float> in(4 * 100, 0.1f), out(2 * 100);
    CHECK(allocationsDuring([&] {
              convolver.publish(builder);
              convolver.process(in.data(), out.data(), 100);
          }) == 0);

    //  Catches: a request or a build on the decoder that allocates -- the audio and worker threads share neither.
    BinauralDecoder decoder;
    REQUIRE(decoder.prepare(m, design, 32, 3, 5));
    decoder.work();
    CHECK(allocationsDuring([&] {
              decoder.request(0.3, 1.05);
              decoder.work();
              decoder.process(in.data(), out.data(), 100);
          }) == 0);

    //  Catches: a loudspeaker publish that sizes its rows on the fly, or a render that gathers a frame into a buffer.
    Layout ring;
    for (int i = 0; i < 6; ++i) {
        Loudspeaker s;
        s.azimuthDeg = 60.0 * i;
        s.channel = i + 1;
        ring.speakers.push_back(s);
    }
    SpeakerDesign speakerDesign;
    REQUIRE(designAllrad(ring, 1, Weighting::Basic, speakerDesign));
    SpeakerRenderer speakers;
    REQUIRE(speakers.prepare(1, 6, 48000.0));
    std::vector<float> speakerOut(6 * 100);
    CHECK(allocationsDuring([&] {
              speakers.publish(speakerDesign);
              speakers.process(in.data(), 4, speakerOut.data(), 6, 100);
              speakers.publish(speakerDesign);  // a second design: the swap path copies into storage it owns
              speakers.process(in.data(), 4, speakerOut.data(), 6, 100);
          }) == 0);

    //  Catches: a turn that builds its blocks or its scratch on the fly while gliding.
    HeadRotation turn;
    REQUIRE(turn.prepare(1, 48000.0));
    std::vector<float> turned(4 * 100);
    CHECK(allocationsDuring([&] {
              turn.setOrientation(rotationAbout({0, 0, 1}, 1.0));
              turn.process(in.data(), turned.data(), 100);
              turn.process(in.data(), turned.data(), 100);
          }) == 0);
}

}  // namespace bambi
