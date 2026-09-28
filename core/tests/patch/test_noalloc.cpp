// SPDX-License-Identifier: GPL-3.0-or-later
#include <atomic>
#include <cstdlib>
#include <functional>
#include <new>

#include "../doctest.h"
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
 *  it with a std::string allocates once it passes 22 characters -- which "region1.band_elevation"
 *  reaches exactly. Nothing catches that unless something counts allocations directly.
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
    //  Catches: build any key with `prefix + "." + field` again and the region keys allocate.
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

}  // namespace bambi
