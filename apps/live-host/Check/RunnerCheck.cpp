// SPDX-License-Identifier: GPL-3.0-or-later
//
//  bambi-live-host-check: the runner plays a plan through the suite's real installed plugins as a direct render of
//  the same chain does, at every block size. Two tracks, each a pair of the loop device into an Encoder; the first
//  through an Echo; both sent to one return with a Reverb, at levels of their own; Main into the Decoder. Every
//  render gets freshly loaded instances, so each starts from the same state as the others.
//
//    bambi-live-host-check
//
//  A second plan puts a stand-in insert that delays by 100 samples, and says so, on the first track: the runner must
//  then delay the second by as much where they meet, which the real plugins -- none reports a latency but the
//  Decoder, which comes last -- never ask of it.
//
//  A third plays a scripted transport -- a start, a new tempo, a locate, a stop -- through the projector, with a
//  stand-in insert that records what its playhead says at every piece: that must be what an independent projector
//  fed the same messages says at the same samples, at every block size.
//
//  The direct renders are wired by hand here, without the runner, so the two can disagree.

#include <cmath>
#include <cstdio>
#include <juce_audio_processors/juce_audio_processors.h>
#include <random>
#include <vector>

#include "../Source/Plugins.h"
#include "../Source/Runner.h"
#include "bambi/live/graph.hpp"
#include "bambi/live/transport.hpp"

using namespace bambi;
using bambi::livehost::Role;

namespace {

constexpr int kOrder = 3, kField = 16, kMaxBlock = 1024;
constexpr double kRate = 48000.0;
constexpr int kLength = 96000;  // two seconds
constexpr float kLevelA = 0.5f, kLevelB = 0.25f;
constexpr int kDeviceBlock = 4096;  // a device may hand over more than the runner was prepared for

struct Set {
    std::vector<std::unique_ptr<juce::AudioPluginInstance>> plugins;  // Encoder A, Encoder B, Echo, Reverb, Decoder
    bool ok{true};
};

Set load() {
    Set s;
    const auto add = [&](const char* name, Role role) {
        auto loaded = livehost::loadInstalled(name, role, kOrder, kRate, kMaxBlock);
        if (loaded.plugin == nullptr) {
            std::printf("  %s\n", loaded.status.toRawUTF8());
            s.ok = false;
        }
        s.plugins.push_back(std::move(loaded.plugin));
    };
    add("bambi Encoder", Role::Encoder);
    add("bambi Encoder", Role::Encoder);
    add("bambi Echo", Role::Effect);
    add("bambi Reverb", Role::Effect);
    add("bambi Decoder", Role::Decoder);
    return s;
}

//  Two tracks of low noise in bursts, each its own stereo pair of the loop device.
juce::AudioBuffer<float> signal() {
    juce::AudioBuffer<float> b(4, kLength);
    std::mt19937 random(5);
    std::uniform_real_distribution<float> noise(-0.1f, 0.1f);
    for (int i = 0; i < kLength; ++i) {
        const bool burstA = (i / 6000) % 3 == 0, burstB = (i / 9000) % 2 == 1;
        for (int ch = 0; ch < 4; ++ch) b.setSample(ch, i, ((ch < 2 ? burstA : burstB) ? noise(random) : 0.0f));
    }
    return b;
}

live::Plan plan() {
    live::Plan p;
    p.slots = {{0}, {0}, {0}, {0}, {0}};  // latencies, filled in from the instances
    live::FieldNode a, b;
    a.track = 1;
    a.pair = 0;
    a.encoder = 0;
    a.inserts = {2};
    b.track = 2;
    b.pair = 1;
    b.encoder = 1;
    p.nodes = {a, b};
    p.returns = {live::Return{{3}}};
    p.decoder = 4;
    return p;
}

juce::AudioBuffer<float> throughRunner(int block) {
    Set s = load();
    if (!s.ok) return {};
    live::Plan p = plan();
    for (std::size_t i = 0; i < s.plugins.size(); ++i) p.slots[i].latency = s.plugins[i]->getLatencySamples();
    std::string error;
    const auto compiled = live::compile(p, &error);
    if (!compiled) {
        std::printf("  the plan: %s\n", error.c_str());
        return {};
    }
    std::vector<juce::AudioProcessor*> slots;
    for (auto& plugin : s.plugins) slots.push_back(plugin.get());
    livehost::Runner runner;
    juce::String why;
    if (!runner.prepare(*compiled, slots, 2, {kOrder, kMaxBlock, 4, 2}, why)) {
        std::printf("  the runner: %s\n", why.toRawUTF8());
        return {};
    }
    runner.setLevel(0, kLevelA);  // node 0 to return 0
    runner.setLevel(1, kLevelB);  // node 1 to return 0
    const juce::AudioBuffer<float> in = signal();
    juce::AudioBuffer<float> out(2, kLength), loop(4, kDeviceBlock), block2(2, kDeviceBlock);
    for (int at = 0; at < kLength; at += block) {
        const int n = std::min(block, kLength - at);
        for (int ch = 0; ch < 4; ++ch) loop.copyFrom(ch, 0, in, ch, at, n);
        runner.process(loop, block2, n);
        for (int ch = 0; ch < 2; ++ch) out.copyFrom(ch, at, block2, ch, 0, n);
    }
    return out;
}

//  The same chain, by hand: each plugin over its own buffer, the sums written out. No delays: of these plugins only
//  the Decoder reports a latency, and nothing comes after it.
juce::AudioBuffer<float> direct() {
    Set s = load();
    if (!s.ok) return {};
    const juce::AudioBuffer<float> in = signal();
    juce::AudioBuffer<float> out(2, kLength);
    juce::MidiBuffer midi;
    constexpr int block = 512;
    juce::AudioBuffer<float> encA(18, block), encB(18, block), echo(18, block), reverb(18, block), decoder(16, block);
    for (int at = 0; at < kLength; at += block) {
        const int n = std::min(block, kLength - at);
        for (auto* b : {&encA, &encB, &echo, &reverb, &decoder}) {
            b->setSize(b->getNumChannels(), n, false, false, true);
            b->clear();
        }
        for (int ch = 0; ch < 2; ++ch) {
            encA.copyFrom(ch, 0, in, ch, at, n);
            encB.copyFrom(ch, 0, in, 2 + ch, at, n);
        }
        s.plugins[0]->processBlock(encA, midi);
        s.plugins[1]->processBlock(encB, midi);
        for (int ch = 0; ch < kField; ++ch) echo.copyFrom(ch, 0, encA, ch, 0, n);
        s.plugins[2]->processBlock(echo, midi);  // the insert on track A
        for (int ch = 0; ch < kField; ++ch) {
            reverb.copyFrom(ch, 0, echo.getReadPointer(ch), n, kLevelA);
            reverb.addFrom(ch, 0, encB, ch, 0, n, kLevelB);
        }
        s.plugins[3]->processBlock(reverb, midi);  // the return
        for (int ch = 0; ch < kField; ++ch) {
            decoder.copyFrom(ch, 0, echo, ch, 0, n);
            decoder.addFrom(ch, 0, encB, ch, 0, n);
            decoder.addFrom(ch, 0, reverb, ch, 0, n);
        }
        s.plugins[4]->processBlock(decoder, midi);
        for (int ch = 0; ch < 2; ++ch) out.copyFrom(ch, at, decoder, ch, 0, n);
    }
    return out;
}

//  A stand-in insert: the field through, `delay` samples late, its latency reported as such.
class DelayBy final : public juce::AudioProcessor {
public:
    explicit DelayBy(int delay)
        : AudioProcessor(BusesProperties()
                             .withInput("in", juce::AudioChannelSet::ambisonic(kOrder))
                             .withOutput("out", juce::AudioChannelSet::ambisonic(kOrder))),
          delay_(delay) {
        setLatencySamples(delay);
    }
    void prepareToPlay(double, int) override {
        memory_.setSize(kField, delay_);
        memory_.clear();
        at_ = 0;
    }
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer&) override {
        for (int i = 0; i < b.getNumSamples(); ++i) {
            for (int ch = 0; ch < kField; ++ch) {
                const float in = b.getSample(ch, i);
                b.setSample(ch, i, memory_.getSample(ch, at_));
                memory_.setSample(ch, at_, in);
            }
            if (++at_ == delay_) at_ = 0;
        }
    }
    const juce::String getName() const override { return "delay by"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

private:
    int delay_;
    juce::AudioBuffer<float> memory_;
    int at_{0};
};

constexpr int kDelay = 100;

//  Track A through the stand-in, track B straight; no return.
juce::AudioBuffer<float> delayedThroughRunner(int block) {
    Set s = load();
    if (!s.ok) return {};
    DelayBy insert(kDelay);
    insert.prepareToPlay(kRate, kMaxBlock);
    live::Plan p;
    p.slots = {{0}, {0}, {kDelay}, {s.plugins[4]->getLatencySamples()}};
    live::FieldNode a, b;
    a.track = 1;
    a.pair = 0;
    a.encoder = 0;
    a.inserts = {2};
    b.track = 2;
    b.pair = 1;
    b.encoder = 1;
    p.nodes = {a, b};
    p.decoder = 3;
    const auto compiled = live::compile(p);
    if (!compiled) return {};
    livehost::Runner runner;
    juce::String why;
    if (!runner.prepare(*compiled, {s.plugins[0].get(), s.plugins[1].get(), &insert, s.plugins[4].get()}, 0,
                        {kOrder, kMaxBlock, 4, 2}, why))
        return {};
    const juce::AudioBuffer<float> in = signal();
    juce::AudioBuffer<float> out(2, kLength), loop(4, kDeviceBlock), outBlock(2, kDeviceBlock);
    for (int at = 0; at < kLength; at += block) {
        const int n = std::min(block, kLength - at);
        for (int ch = 0; ch < 4; ++ch) loop.copyFrom(ch, 0, in, ch, at, n);
        runner.process(loop, outBlock, n);
        for (int ch = 0; ch < 2; ++ch) out.copyFrom(ch, at, outBlock, ch, 0, n);
    }
    return out;
}

//  The same by hand: both tracks' fields 100 samples late, A through the stand-in, B by a delay written out here.
juce::AudioBuffer<float> delayedDirect() {
    Set s = load();
    if (!s.ok) return {};
    DelayBy insert(kDelay);
    insert.prepareToPlay(kRate, 512);
    const juce::AudioBuffer<float> in = signal();
    juce::AudioBuffer<float> out(2, kLength), late(kField, kLength + kDelay);
    late.clear();
    juce::MidiBuffer midi;
    constexpr int block = 512;
    juce::AudioBuffer<float> encA(18, block), encB(18, block), decoder(16, block);
    for (int at = 0; at < kLength; at += block) {
        const int n = std::min(block, kLength - at);
        for (auto* b : {&encA, &encB, &decoder}) {
            b->setSize(b->getNumChannels(), n, false, false, true);
            b->clear();
        }
        for (int ch = 0; ch < 2; ++ch) {
            encA.copyFrom(ch, 0, in, ch, at, n);
            encB.copyFrom(ch, 0, in, 2 + ch, at, n);
        }
        s.plugins[0]->processBlock(encA, midi);
        s.plugins[1]->processBlock(encB, midi);
        juce::AudioBuffer<float> a(encA.getArrayOfWritePointers(), kField, n);
        insert.processBlock(a, midi);
        for (int ch = 0; ch < kField; ++ch) late.addFrom(ch, at + kDelay, encB, ch, 0, n);  // B, 100 late
        for (int ch = 0; ch < kField; ++ch) {
            decoder.copyFrom(ch, 0, encA, ch, 0, n);
            decoder.addFrom(ch, 0, late, ch, at, n);
        }
        s.plugins[4]->processBlock(decoder, midi);
        for (int ch = 0; ch < 2; ++ch) out.copyFrom(ch, at, decoder, ch, 0, n);
    }
    return out;
}

//  A stand-in insert that passes the field through and records, at every processBlock, what its playhead says.
class Recorder final : public juce::AudioProcessor {
public:
    Recorder()
        : AudioProcessor(BusesProperties()
                             .withInput("in", juce::AudioChannelSet::ambisonic(kOrder))
                             .withOutput("out", juce::AudioChannelSet::ambisonic(kOrder))) {}
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {
        live::Playhead p;
        p.timeline = -1;  // nothing said
        if (auto* head = getPlayHead())
            if (const auto info = head->getPosition()) {
                p.playing = info->getIsPlaying();
                p.timeline = info->getTimeInSamples().orFallback(-1);
                p.ppq = info->getPpqPosition().orFallback(-1.0);
                p.bpm = info->getBpm().orFallback(-1.0);
            }
        seen.push_back(p);
    }
    const juce::String getName() const override { return "recorder"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

    std::vector<live::Playhead> seen;
};

constexpr int kTimed = 131072;  // 32 blocks of 4096: every message falls on a block start at every size tried

struct Timed {
    live::TransportMessage m;
    std::int64_t at;
};

//  Live starts at 0 at 120 bpm; at 49152 it is at 2.048 and changes to 90; at 98304 it jumps to 32; at 122880 it
//  stops, at 32.768.
const std::vector<Timed> kScript{{{true, 0.0, 120.0}, 0},
                                 {{true, 2.048, 90.0}, 49152},
                                 {{true, 32.0, 90.0}, 98304},
                                 {{false, 32.768, 90.0}, 122880}};

live::ProjectorSettings clockSettings() {
    live::ProjectorSettings s;
    s.sampleRate = kRate;
    return s;
}

//  What each piece's playhead said, through a real Encoder, the recorder and a real Decoder.
std::vector<live::Playhead> timedThroughRunner(int block, std::vector<std::int64_t>& starts) {
    Set s = load();
    if (!s.ok) return {};
    Recorder recorder;
    live::Plan p;
    p.slots = {{0}, {0}, {s.plugins[4]->getLatencySamples()}};
    live::FieldNode a;
    a.track = 1;
    a.pair = 0;
    a.encoder = 0;
    a.inserts = {1};
    p.nodes = {a};
    p.decoder = 2;
    const auto compiled = live::compile(p);
    if (!compiled) return {};
    live::TransportProjector clock(clockSettings());
    livehost::Runner runner;
    juce::String why;
    if (!runner.prepare(*compiled, {s.plugins[0].get(), &recorder, s.plugins[4].get()}, 0,
                        {kOrder, kMaxBlock, 4, 2, kRate}, why))
        return {};
    runner.setClock(&clock);
    recorder.seen.reserve(kTimed + 16);
    juce::AudioBuffer<float> loop(4, kDeviceBlock), out(2, kDeviceBlock);
    loop.clear();
    std::size_t next = 0;
    for (int at = 0; at < kTimed; at += block) {
        //  a message that came in during the last block is handed over before this one, at its own sample
        while (next < kScript.size() && kScript[next].at <= at) {
            clock.receive(kScript[next].m, kScript[next].at);
            ++next;
        }
        const int n = std::min(block, kTimed - at);
        for (int piece = 0; piece < n; piece += kMaxBlock) starts.push_back(at + piece);
        runner.process(loop, out, n);
    }
    return recorder.seen;
}

double largestDifference(const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b) {
    double most = 0.0;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < kLength; ++i)
            most = std::max(most, std::abs(static_cast<double>(a.getSample(ch, i)) - b.getSample(ch, i)));
    return most;
}

double peak(const juce::AudioBuffer<float>& a) {
    return std::max(a.getMagnitude(0, 0, kLength), a.getMagnitude(1, 0, kLength));
}

}  // namespace

int main() {
    juce::ScopedJuceInitialiser_GUI juce;  // the plugins' editors and message thread exist even unseen
    std::printf("the direct render, block 512\n");
    const auto reference = direct();
    if (reference.getNumSamples() == 0) return 1;
    std::printf("  peak %.4f\n", peak(reference));
    if (peak(reference) < 1e-4) {
        std::printf("silent: nothing to compare\n");
        return 1;
    }
    int failed = 0;
    for (const int block : {1, 7, 64, 512, 1024, 1500, 4096}) {
        const auto got = throughRunner(block);
        if (got.getNumSamples() == 0) return 1;
        const double diff = largestDifference(got, reference);
        const bool ok = diff <= 1e-6;
        failed += ok ? 0 : 1;
        std::printf("the runner, block %4d: largest difference %.3g %s\n", block, diff, ok ? "" : "  FAIL");
    }
    std::printf("the delayed plan, by hand\n");
    const auto delayedReference = delayedDirect();
    if (delayedReference.getNumSamples() == 0) return 1;
    for (const int block : {1, 7, 64, 99, 100, 101, 512, 1024, 1500, 4096}) {
        const auto got = delayedThroughRunner(block);
        if (got.getNumSamples() == 0) return 1;
        const double diff = largestDifference(got, delayedReference);
        const bool ok = diff <= 1e-6;
        failed += ok ? 0 : 1;
        std::printf("the runner, delayed, block %4d: largest difference %.3g %s\n", block, diff, ok ? "" : "  FAIL");
    }
    std::printf("the transport, scripted\n");
    std::vector<std::vector<live::Playhead>> runs;
    std::vector<std::vector<std::int64_t>> runStarts;
    for (const int block : {1, 64, 512, 1024, 4096}) {
        std::vector<std::int64_t> starts;
        auto seen = timedThroughRunner(block, starts);
        //  what an independent projector, fed the same messages at the same moments, says at the same samples
        live::TransportProjector reference(clockSettings());
        std::size_t next = 0, wrong = 0;
        for (std::size_t i = 0; i < starts.size(); ++i) {
            const std::int64_t blockStart = starts[i] - starts[i] % block;
            while (next < kScript.size() && kScript[next].at <= blockStart) {
                reference.receive(kScript[next].m, kScript[next].at);
                ++next;
            }
            const live::Playhead want = reference.reading(starts[i]);
            if (i >= seen.size() || seen[i].playing != want.playing || seen[i].timeline != want.timeline ||
                seen[i].ppq != want.ppq || seen[i].bpm != want.bpm)
                ++wrong;
        }
        const bool ok = seen.size() == starts.size() && wrong == 0;
        failed += ok ? 0 : 1;
        std::printf("the playhead, block %4d: %zu pieces, %zu not as the projector says %s\n", block, starts.size(),
                    wrong, ok ? "" : "  FAIL");
        runs.push_back(std::move(seen));
        runStarts.push_back(std::move(starts));
    }
    //  and the same wherever two block sizes play a piece from the same sample
    std::size_t differ = 0;
    for (std::size_t r = 1; r < runs.size(); ++r)
        for (std::size_t i = 0; i < runStarts[r].size() && i < runs[r].size(); ++i) {
            const auto& base = runStarts[0];
            const auto at = runStarts[r][i];
            if (at >= static_cast<std::int64_t>(base.size())) continue;  // block 1 plays a piece from every sample
            const auto& a = runs[0][static_cast<std::size_t>(at)];
            const auto& b = runs[r][i];
            if (a.playing != b.playing || a.timeline != b.timeline || a.ppq != b.ppq || a.bpm != b.bpm) ++differ;
        }
    failed += differ == 0 ? 0 : 1;
    std::printf("the playhead at the same sample, across block sizes: %zu differ %s\n", differ,
                differ == 0 ? "" : "  FAIL");

    std::printf(failed == 0 ? "the runner plays both plans as the direct renders do, at every block size\n"
                            : "%d renders differ\n",
                failed);
    return failed == 0 ? 0 : 1;
}
