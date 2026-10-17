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
//  A fourth swaps one program for another while it plays: for an identical one, the output must be the unswapped
//  render faded out and back in, exactly; for one that adds a track, the track must be heard after it and not
//  before; and the program swapped out, with what it kept alive, must be freed by tidy() and never in a block.
//
//  The direct renders are wired by hand here, without the runner, so the two can disagree.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <juce_audio_processors/juce_audio_processors.h>
#include <random>
#include <vector>

#include "../Source/Plugins.h"
#include "../Source/Runner.h"
#include "bambi/decode/remote.hpp"
#include "bambi/live/graph.hpp"
#include "bambi/live/transport.hpp"

using namespace bambi;
using bambi::SnapshotCensus;
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
    a.input = 0;
    a.encoder = 0;
    a.inserts = {2};
    b.track = 2;
    b.input = 2;
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
    if (!runner.prepare(*compiled, slots, compiled->levels, {kOrder, kMaxBlock, 4, 2}, why)) {
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
//  A stand-in plugin: everything a processor must say that none of the stand-ins has anything to say about.
class StandIn : public juce::AudioProcessor {
public:
    using AudioProcessor::AudioProcessor;
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    const juce::String getName() const override { return "stand-in"; }
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
};

class DelayBy final : public StandIn {
public:
    explicit DelayBy(int delay)
        : StandIn(BusesProperties()
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
    a.input = 0;
    a.encoder = 0;
    a.inserts = {2};
    b.track = 2;
    b.input = 2;
    b.encoder = 1;
    p.nodes = {a, b};
    p.decoder = 3;
    const auto compiled = live::compile(p);
    if (!compiled) return {};
    livehost::Runner runner;
    juce::String why;
    if (!runner.prepare(*compiled, {s.plugins[0].get(), s.plugins[1].get(), &insert, s.plugins[4].get()},
                        compiled->levels, {kOrder, kMaxBlock, 4, 2}, why))
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
class Recorder final : public StandIn {
public:
    Recorder()
        : StandIn(BusesProperties()
                      .withInput("in", juce::AudioChannelSet::ambisonic(kOrder))
                      .withOutput("out", juce::AudioChannelSet::ambisonic(kOrder))) {}
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
    a.input = 0;
    a.encoder = 0;
    a.inserts = {1};
    p.nodes = {a};
    p.decoder = 2;
    const auto compiled = live::compile(p);
    if (!compiled) return {};
    live::TransportProjector clock(clockSettings());
    livehost::Runner runner;
    juce::String why;
    if (!runner.prepare(*compiled, {s.plugins[0].get(), &recorder, s.plugins[4].get()}, compiled->levels,
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
        //  the runner plays in pieces of at most kMaxBlock that never cross a point of its level grid
        for (int piece = 0; piece < n;) {
            starts.push_back(at + piece);
            const int grid = livehost::Runner::kLevelGrid;
            piece += std::min({kMaxBlock, n - piece, grid - (at + piece) % grid});
        }
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

//  One Encoder on `pair`, or one on each of `pairs`, into the Decoder: slot 0 and 1 are the Set's two Encoders.
live::Plan encoding(std::vector<int> pairs) {
    live::Plan p;
    p.slots = {{0}, {0}, {0}, {0}, {0}};
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        live::FieldNode n;
        n.track = static_cast<live::TrackId>(i + 1);
        n.input = 2 * pairs[i];
        n.encoder = static_cast<int>(i);
        p.nodes.push_back(n);
    }
    p.decoder = 4;
    return p;
}

constexpr int kSwapAt = 24000;  // a multiple of every block size the swap is played at; 100 does not divide the fade

struct Swapped {
    juce::AudioBuffer<float> out;
    bool keptThroughSwap{false};  // what the first program kept, alive once the second played
    bool freedByTidy{false};      // and gone after tidy()
};

//  `first` played, `second` swapped in before the block at kSwapAt; `second` empty: no swap.
Swapped swapRender(int block, const live::Plan& first, const live::Plan* second, const juce::AudioBuffer<float>& in) {
    Swapped r;
    Set s = load();
    if (!s.ok) return r;
    live::Plan played = first;
    for (std::size_t i = 0; i < s.plugins.size(); ++i) played.slots[i].latency = s.plugins[i]->getLatencySamples();
    std::vector<juce::AudioProcessor*> slots;
    for (auto& plugin : s.plugins) slots.push_back(plugin.get());
    livehost::Runner runner;
    juce::String why;
    auto kept = std::make_shared<int>(1);
    const std::weak_ptr<int> watch = kept;
    if (!runner.prepare(*live::compile(played), slots, live::compile(played)->levels, {kOrder, kMaxBlock, 4, 2}, why,
                        {kept})) {
        std::printf("  the runner: %s\n", why.toRawUTF8());
        return r;
    }
    kept.reset();
    r.out.setSize(2, kLength);
    juce::AudioBuffer<float> loop(4, kDeviceBlock), part(2, kDeviceBlock);
    for (int at = 0; at < kLength; at += block) {
        if (at == kSwapAt && second != nullptr) {
            for (auto& plugin : s.plugins) runner.ready(plugin.get());
            live::Plan next = *second;
            for (std::size_t i = 0; i < s.plugins.size(); ++i)
                next.slots[i].latency = s.plugins[i]->getLatencySamples();
            if (!runner.swap(*live::compile(next), slots, live::compile(next)->levels, why))
                std::printf("  the swap: %s\n", why.toRawUTF8());
        }
        const int n = std::min(block, kLength - at);
        for (int ch = 0; ch < 4; ++ch) loop.copyFrom(ch, 0, in, ch, at, n);
        runner.process(loop, part, n);
        for (int ch = 0; ch < 2; ++ch) r.out.copyFrom(ch, at, part, ch, 0, n);
    }
    r.keptThroughSwap = !watch.expired();
    runner.tidy();
    r.freedByTidy = watch.expired();
    return r;
}

//  Low noise on the pairs asked for, silence on the others.
juce::AudioBuffer<float> noiseOn(std::vector<int> pairs) {
    juce::AudioBuffer<float> b(4, kLength);
    b.clear();
    std::mt19937 random(9);
    std::uniform_real_distribution<float> noise(-0.1f, 0.1f);
    for (const int pair : pairs)
        for (int i = 0; i < kLength; ++i)
            for (int ch = 2 * pair; ch < 2 * pair + 2; ++ch) b.setSample(ch, i, noise(random));
    return b;
}

//  Stand-ins that make a send's gain readable: one that puts its track's left channel on every channel of the field,
//  where an Encoder would encode it, and one that gives the field's first two channels out, where a Decoder would.
class Spread final : public StandIn {
public:
    Spread()
        : StandIn(BusesProperties()
                      .withInput("in", juce::AudioChannelSet::stereo())
                      .withOutput("out", juce::AudioChannelSet::ambisonic(kOrder))) {}
    void processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer&) override {
        for (int ch = 1; ch < kField; ++ch) b.copyFrom(ch, 0, b, 0, 0, b.getNumSamples());
    }
};

class PassOut final : public StandIn {
public:
    PassOut()
        : StandIn(BusesProperties()
                      .withInput("in", juce::AudioChannelSet::ambisonic(kOrder))
                      .withOutput("out", juce::AudioChannelSet::stereo())) {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
};

constexpr float kLevel = 0.1f;  // the track, constant on its left channel

//  One track through stand-ins into Main, or with `delayed` a second through a stand-in that delays by kDelay, so
//  the first is delayed as much where they meet; constant on their left channels. With `swap`, the same plan
//  swapped in before the block at kSwapAt. Main's first channel, sample by sample.
std::vector<float> standInSwap(int block, bool swap, bool delayed) {
    Spread a, b;
    DelayBy delay(kDelay);
    PassOut out;
    delay.prepareToPlay(kRate, kMaxBlock);
    live::Plan p;
    live::FieldNode first;
    first.track = 1;
    first.input = 0;
    first.encoder = 0;
    p.nodes = {first};
    std::vector<juce::AudioProcessor*> slots{&a};
    if (delayed) {
        live::FieldNode second;
        second.track = 2;
        second.input = 2;
        second.encoder = 1;
        second.inserts = {2};
        p.nodes.push_back(second);
        slots.push_back(&b);
        slots.push_back(&delay);
    }
    p.decoder = static_cast<int>(slots.size());
    slots.push_back(&out);
    p.slots.assign(slots.size(), {});
    if (delayed) p.slots[2].latency = kDelay;
    const auto compiled = live::compile(p);
    livehost::Runner runner;
    juce::String why;
    if (!compiled || !runner.prepare(*compiled, slots, compiled->levels, {kOrder, kMaxBlock, 4, 2, kRate}, why))
        return {};
    std::vector<float> got;
    juce::AudioBuffer<float> loop(4, kDeviceBlock), part(2, kDeviceBlock);
    loop.clear();
    for (int i = 0; i < kDeviceBlock; ++i) {
        loop.setSample(0, i, kLevel);
        loop.setSample(2, i, kLevel);
    }
    for (int at = 0; at < kLength; at += block) {
        if (swap && at == kSwapAt) (void)runner.swap(*compiled, slots, compiled->levels, why);
        const int n = std::min(block, kLength - at);
        runner.process(loop, part, n);
        for (int i = 0; i < n; ++i) got.push_back(part.getSample(0, i));
    }
    return got;
}

//  An insert that plays its sidechain's left channel in place of the field it is given: what reaches its sidechain,
//  made audible.
class Side final : public StandIn {
public:
    Side()
        : StandIn(BusesProperties()
                      .withInput("in", juce::AudioChannelSet::ambisonic(kOrder))
                      .withOutput("out", juce::AudioChannelSet::ambisonic(kOrder))
                      .withInput("sidechain", juce::AudioChannelSet::stereo())) {}
    void processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer&) override {
        for (int ch = 0; ch < kField; ++ch) b.copyFrom(ch, 0, b, kField, 0, b.getNumSamples());
    }
};

//  An encoder that puts its sidechain's left channel on every channel of the field, where Spread puts its track's.
class SpreadSide final : public StandIn {
public:
    SpreadSide()
        : StandIn(BusesProperties()
                      .withInput("in", juce::AudioChannelSet::stereo())
                      .withOutput("out", juce::AudioChannelSet::ambisonic(kOrder))
                      .withInput("sidechain", juce::AudioChannelSet::stereo())) {}
    void processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer&) override {
        const juce::AudioBuffer<float> side(b.getArrayOfWritePointers() + 2, 1, b.getNumSamples());
        for (int ch = kField - 1; ch >= 0; --ch) b.copyFrom(ch, 0, side, 0, 0, b.getNumSamples());
    }
};

//  One track into Main, its sidechain hearing `pair` (-1: none) -- the insert's, or with `encoder` the track's own
//  Encoder's; the track on pair 0 at kLevel, pair 1 at three times that. Main's first channel, sample by sample.
std::vector<float> sidechained(int block, int pair, bool encoder = false) {
    Spread a;
    SpreadSide b;
    Side side;
    PassOut out;
    live::Plan p;
    live::FieldNode n;
    n.track = 1;
    n.input = 0;
    n.encoder = 0;
    if (!encoder) n.inserts = {1};
    p.nodes = {n};
    const int heard = pair < 0 ? -1 : 2 * pair;  // the pair's first channel
    p.slots = {{0, encoder ? heard : -1}, {0, encoder ? -1 : heard}, {0, -1}};
    p.decoder = 2;
    const auto compiled = live::compile(p);
    livehost::Runner runner;
    juce::String why;
    juce::AudioProcessor* first = encoder ? static_cast<juce::AudioProcessor*>(&b) : &a;
    if (!compiled ||
        !runner.prepare(*compiled, {first, &side, &out}, compiled->levels, {kOrder, kMaxBlock, 4, 2, kRate}, why)) {
        std::printf("  the runner: %s\n", why.toRawUTF8());
        return {};
    }
    std::vector<float> got;
    juce::AudioBuffer<float> loop(4, kDeviceBlock), part(2, kDeviceBlock);
    loop.clear();
    for (int i = 0; i < kDeviceBlock; ++i) {
        loop.setSample(0, i, kLevel);
        loop.setSample(2, i, 3.0f * kLevel);
    }
    for (int at = 0; at < kLength; at += block) {
        const int n = std::min(block, kLength - at);
        runner.process(loop, part, n);
        for (int i = 0; i < n; ++i) got.push_back(part.getSample(0, i));
    }
    return got;
}

//  An encoder that puts its track's two channels on the field's first two, where PassOut gives them out.
class Pass final : public StandIn {
public:
    Pass()
        : StandIn(BusesProperties()
                      .withInput("in", juce::AudioChannelSet::stereo())
                      .withOutput("out", juce::AudioChannelSet::ambisonic(kOrder))) {}
    void processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer&) override {
        for (int ch = 2; ch < kField; ++ch) b.clear(ch, 0, b.getNumSamples());
    }
};

//  One source arriving `width` channels wide from input row channel `first`, each of the row's four channels a
//  constant of its own: what reaches the output's left and right once the start's fade is over.
std::pair<float, float> arrivedAt(int first, int width) {
    Pass a;
    PassOut out;
    live::Plan p;
    live::FieldNode n;
    n.track = 1;
    n.input = first;
    n.width = width;
    n.encoder = 0;
    p.nodes = {n};
    p.slots = {{0, -1}, {0, -1}};
    p.decoder = 1;
    const auto compiled = live::compile(p);
    livehost::Runner runner;
    juce::String why;
    if (!compiled || !runner.prepare(*compiled, {&a, &out}, compiled->levels, {kOrder, kMaxBlock, 4, 2, kRate}, why))
        return {-1.0f, -1.0f};
    juce::AudioBuffer<float> row(4, 512), part(2, 512);
    for (int ch = 0; ch < 4; ++ch) juce::FloatVectorOperations::fill(row.getWritePointer(ch), 0.1f * static_cast<float>(ch + 1), 512);
    for (int k = 0; k < 8; ++k) runner.process(row, part, 512);
    return {part.getSample(0, 511), part.getSample(1, 511)};
}

//  The swap's fade at `k` samples after it was asked for, with no delay to hold through: down, then up.
double fadeAt(int k) {
    const int f = livehost::Runner::kFade;
    if (k < 0 || k >= 2 * f) return 1.0;
    return k < f ? 1.0 - static_cast<double>(k) / f : static_cast<double>(k - f) / f;
}
//  When the send's level is set, and to what: before the block at each sample, which every block size tried
//  starts a block at; neither is a point of the level grid.
const std::vector<std::pair<int, float>> kLevels{{0, 1.0f}, {24000, 0.25f}, {48000, 0.75f}};
constexpr int kRampLength = 64000;

//  The send's gain the output says, sample by sample: one track into Main and, at the level, into an empty return.
//  `swapAt`, when not 0: the same plan swapped in before the block there, its level set to `swapLevel` at once.
std::vector<float> sendGains(int block, int swapAt = 0, float swapLevel = 0.0f) {
    Spread spread;
    PassOut out;
    live::Plan p;
    p.slots = {{0}, {0}};
    live::FieldNode a;
    a.track = 1;
    a.input = 0;
    a.encoder = 0;
    p.nodes = {a};
    p.returns = {live::Return{}};
    p.decoder = 1;
    const auto compiled = live::compile(p);
    if (!compiled) return {};
    livehost::Runner runner;
    juce::String why;
    if (!runner.prepare(*compiled, {&spread, &out}, compiled->levels, {kOrder, kMaxBlock, 2, 2, kRate}, why)) {
        std::printf("  the runner: %s\n", why.toRawUTF8());
        return {};
    }
    std::vector<float> gains;
    juce::AudioBuffer<float> loop(2, kDeviceBlock), part(2, kDeviceBlock);
    loop.clear();
    for (int i = 0; i < kDeviceBlock; ++i) loop.setSample(0, i, kLevel);
    std::size_t next = 0;
    for (int at = 0; at < kRampLength; at += block) {
        while (next < kLevels.size() && kLevels[next].first <= at) runner.setLevel(0, kLevels[next++].second);
        if (swapAt != 0 && at == swapAt) {
            (void)runner.swap(*compiled, {&spread, &out}, compiled->levels, why);
            runner.setLevel(0, swapLevel);
        }
        const int n = std::min(block, kRampLength - at);
        runner.process(loop, part, n);
        //  the track, plus the send; through a swap's fade, the output over the fade's gain, which the track has
        //  at the source and again at the output
        for (int i = 0; i < n; ++i) {
            const int k = at + i - swapAt - livehost::Runner::kFade;
            const float fade = swapAt != 0 && k >= 0 && k < livehost::Runner::kFade
                                   ? static_cast<float>(k) / static_cast<float>(livehost::Runner::kFade)
                                   : 1.0f;
            gains.push_back(fade > 0.0f ? part.getSample(0, i) / (kLevel * fade * fade) - 1.0f : 0.0f);
        }
    }
    return gains;
}

//  The same rule written out on its own: at each grid point the level last set is taken, and ramped to over the
//  next interval; at the very first, it is simply the level.
float expectedGain(int sample) {
    const int grid = livehost::Runner::kLevelGrid;
    const auto levelAt = [](int t) {
        float l = 1.0f;
        for (const auto& [when, level] : kLevels)
            if (when <= t) l = level;
        return l;
    };
    const int start = sample - sample % grid;
    const float to = levelAt(start);
    const float from = start == 0 ? to : levelAt(start - grid);
    return from + (to - from) * static_cast<float>(sample - start) / static_cast<float>(grid);
}

//  The Decoder on `outputs` channels, fed low noise in W for a quarter second: the channels that carry sound, or
//  none when it would not be laid out that wide.
std::vector<int> decoderHeard(juce::AudioPluginInstance& decoder, int outputs) {
    if (decoder.getTotalNumOutputChannels() != outputs) return {};
    juce::AudioBuffer<float> b(std::max(kField, outputs), 512);
    juce::MidiBuffer midi;
    std::mt19937 random(9);
    std::uniform_real_distribution<float> noise(-0.1f, 0.1f);
    std::vector<float> loudest(static_cast<std::size_t>(outputs), 0.0f);
    for (int block = 0; block < 24; ++block) {
        b.clear();
        for (int i = 0; i < b.getNumSamples(); ++i) b.setSample(0, i, noise(random));
        decoder.processBlock(b, midi);
        for (int ch = 0; ch < outputs; ++ch)
            loudest[static_cast<std::size_t>(ch)] =
                std::max(loudest[static_cast<std::size_t>(ch)], b.getMagnitude(ch, 0, b.getNumSamples()));
    }
    std::vector<int> heard;
    for (int ch = 0; ch < outputs; ++ch)
        if (loudest[static_cast<std::size_t>(ch)] > 1e-4f) heard.push_back(ch);
    return heard;
}

juce::String channelList(const std::vector<int>& channels) {
    juce::StringArray s;
    for (const int ch : channels) s.add(juce::String(ch + 1));
    return s.isEmpty() ? juce::String("none") : s.joinIntoString(" ");
}

//  The Decoder on a bus wider than a pair, as the host asks for one to play speakers beside the headphones.
int wideDecoder() {
    int failed = 0;
    const auto report = [&](const juce::String& what, juce::AudioPluginInstance* decoder, int outputs) {
        const auto heard = decoder != nullptr ? decoderHeard(*decoder, outputs) : std::vector<int>{};
        //  the headphones are the first pair until the Decoder is told otherwise
        const bool ok = heard.size() >= 2 && heard[0] == 0 && heard[1] == 1;
        failed += ok ? 0 : 1;
        std::printf("  %s: sound on %s %s\n", what.toRawUTF8(), channelList(heard).toRawUTF8(), ok ? "" : "  FAIL");
    };
    //  Catches: a wide bus asked for as discrete channels -- JUCE names no VST3 speaker for any past the first, and
    //  the Decoder is refused -- and a bus wider than VST3 has speakers for.
    for (const int outputs : {2, 26, kMaxVst3BusChannels}) {
        auto loaded = livehost::loadInstalled("bambi Decoder", Role::Decoder, kOrder, kRate, kMaxBlock, outputs);
        report(juce::String(outputs) + " out, " + loaded.status, loaded.plugin.get(), outputs);
    }
    //  Catches: a layout changed on a prepared instance without releasing it first, or not prepared again after.
    auto loaded = livehost::loadInstalled("bambi Decoder", Role::Decoder, kOrder, kRate, kMaxBlock, 2);
    if (loaded.plugin != nullptr) {
        loaded.plugin->releaseResources();
        auto layout = loaded.plugin->getBusesLayout();
        layout.outputBuses.getReference(0) = livehost::speakerChannels(26);
        const bool took = loaded.plugin->setBusesLayout(layout);
        loaded.plugin->prepareToPlay(kRate, kMaxBlock);
        report(juce::String("2 out, laid out again as 26") + (took ? "" : " -- refused"), loaded.plugin.get(), 26);
    } else {
        report("2 out, to lay out again", nullptr, 26);
    }
    return failed;
}

//  The Decoder set through its remote interface, as the audio sheet sets it: what it reports, a request it grants,
//  one it refuses, and the outputs heard where the report says.
int remoteDecoder() {
    int failed = 0;
    const auto check = [&](bool ok, const char* what) {
        failed += ok ? 0 : 1;
        std::printf("  %s %s\n", what, ok ? "" : "  FAIL");
    };
    auto loaded = livehost::loadInstalled("bambi Decoder", Role::Decoder, kOrder, kRate, kMaxBlock, 26);
    auto echo = livehost::loadInstalled("bambi Echo", Role::Effect, kOrder, kRate, kMaxBlock);
    if (loaded.plugin == nullptr || echo.plugin == nullptr) {
        check(false, "the Decoder and the Echo load");
        return failed;
    }
    //  Catches: an interface the plugin does not answer, one answered by a plugin that does not offer it, a version
    //  not checked.
    remote::IRemote* decoder = livehost::remoteOf(*loaded.plugin, kDecoderRemoteKind, kDecoderRemoteVersion);
    check(decoder != nullptr, "the Decoder answers the remote interface");
    check(livehost::remoteOf(*echo.plugin, kDecoderRemoteKind, kDecoderRemoteVersion) == nullptr, "the Echo does not");
    check(livehost::remoteOf(*loaded.plugin, kDecoderRemoteKind, kDecoderRemoteVersion + 1) == nullptr &&
              livehost::remoteOf(*loaded.plugin, "echo", kDecoderRemoteVersion) == nullptr,
          "a payload or a version the Decoder does not speak finds nothing");
    if (decoder == nullptr) return failed;
    const auto report = [&] { return reportFrom(Json::parse(livehost::remoteReport(*decoder))); };
    const DecoderReport fresh = report();
    check(fresh.bus == 26 && fresh.headphonesOn && fresh.headphoneChannel == 1 && !fresh.speakersOn,
          "fresh: a bus of 26, the headphones on 1-2, the speakers off");

    //  Catches: a request that does not reach the Decoder's state, a layout not designed after it, the level not
    //  set on the parameter, a granted move not reported.
    DecoderRequest request;
    request.layout = "9+10+3";
    request.speakersOn = true;
    request.levelDb = -6.0;
    check(decoder->remoteRequest(toJson(request).dump(0).c_str()) == Steinberg::kResultOk, "a request is read");
    const DecoderReport granted = report();
    std::vector<int> want{1, 2};
    for (const int c : granted.speakerChannels) want.push_back(c);
    check(granted.speakersOn && granted.layout == "9+10+3" && granted.speakerFirstChannel == 3 &&
              granted.speakerChannels.size() == 22 && granted.speakerChannels.back() == 26 &&
              std::abs(granted.levelDb + 6.0) < 0.01 && !granted.notice.empty(),
          "granted: 9+10+3 lit from channel 3, past the headphones, at -6 dB, the move said");
    std::vector<int> heard = decoderHeard(*loaded.plugin, 26);
    std::printf("    heard on %s\n", channelList(heard).toRawUTF8());
    for (int& c : heard) ++c;
    check(heard == want, "heard on the channels the report gives");

    //  Catches: a refused request applied anyway, or refused without a word.
    DecoderRequest wrong;
    wrong.headphoneChannel = 2;
    decoder->remoteRequest(toJson(wrong).dump(0).c_str());
    const DecoderReport refused = report();
    check(refused.headphoneChannel == 1 && refused.notice == "headphones cannot start on channel 2",
          "refused: headphones on an even channel, said");

    //  Catches: channels the host keeps not reaching the Decoder's placement, or an output left playing on them.
    auto kept = livehost::loadInstalled("bambi Decoder", Role::Decoder, kOrder, kRate, kMaxBlock, 26);
    if (remote::IRemote* k = kept.plugin != nullptr ? livehost::remoteOf(*kept.plugin, kDecoderRemoteKind,
                                                                         kDecoderRemoteVersion)
                                                    : nullptr) {
        DecoderRequest main;
        main.kept = std::vector<int>{1, 2};
        main.deviceEnds = std::vector<int>{16};
        k->remoteRequest(toJson(main).dump(0).c_str());
        const DecoderReport moved = reportFrom(Json::parse(livehost::remoteReport(*k)));
        const std::vector<int> heardKept = decoderHeard(*kept.plugin, 26);
        std::printf("    1-2 kept: heard on %s\n", channelList(heardKept).toRawUTF8());
        check(moved.headphonesOn && moved.headphoneChannel == 3 && moved.kept == std::vector<int>{1, 2} &&
                  heardKept == std::vector<int>{2, 3},
              "1-2 kept for Main: the headphones moved to 3-4, and heard there");
    } else {
        check(false, "a third Decoder answers");
    }

    //  Catches: a widest bus whose last channels -- the last speakers VST3 names -- do not reach the Decoder's.
    auto widest = livehost::loadInstalled("bambi Decoder", Role::Decoder, kOrder, kRate, kMaxBlock, kMaxVst3BusChannels);
    remote::IRemote* far = widest.plugin != nullptr ? livehost::remoteOf(*widest.plugin, kDecoderRemoteKind,
                                                                         kDecoderRemoteVersion)
                                                    : nullptr;
    if (far == nullptr) {
        check(false, "the widest Decoder answers");
        return failed;
    }
    DecoderRequest last;
    last.layout = "9+10+3";
    last.speakerFirstChannel = kMaxVst3BusChannels - 23;
    last.speakersOn = true;
    far->remoteRequest(toJson(last).dump(0).c_str());
    const std::vector<int> heardFar = decoderHeard(*widest.plugin, kMaxVst3BusChannels);
    std::printf("    widest: heard on %s\n", channelList(heardFar).toRawUTF8());
    check(!heardFar.empty() && heardFar.back() == kMaxVst3BusChannels - 1 &&
              heardFar.size() == 2 + reportFrom(Json::parse(livehost::remoteReport(*far))).speakerChannels.size(),
          "widest: 9+10+3 heard up to channel 61");
    return failed;
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

    std::printf("a program swapped while it plays\n");
    const auto one = encoding({0});
    const auto both = encoding({0, 1});
    const auto onFirst = noiseOn({0});
    const auto onSecond = noiseOn({1});
    for (const int block : {1, 64, 100, 1500}) {
        //  Catches: a swap with no fade, a fade on the output only -- an insert or line kept across the swap would
        //  hear a step -- a fade that is not the same on both, or a swap in the wrong place.
        //  For the same plan on stand-ins that keep nothing: the unswapped render, times the fade at the source and
        //  again at the output.
        const auto plainOut = standInSwap(block, false, false);
        const auto sameOut = standInSwap(block, true, false);
        if (plainOut.size() != static_cast<std::size_t>(kLength) || sameOut.size() != plainOut.size()) return 1;
        double off = 0.0;
        for (int i = 0; i < kLength; ++i) {
            const double f = fadeAt(i - kSwapAt);
            off = std::max(
                off, std::abs(sameOut[static_cast<std::size_t>(i)] - f * f * plainOut[static_cast<std::size_t>(i)]));
        }
        const bool faded = off <= 1e-6;
        //  Catches: a program swapped in while the old one's lines and a kept insert still hold what came before --
        //  one path's tail would be heard without the other's. With a delay line, silence is held through it, and
        //  the paths meet again only once both start afresh.
        const auto heldOut = standInSwap(block, true, true);
        if (heldOut.size() != static_cast<std::size_t>(kLength)) return 1;
        float duringHold = 0.0f;
        for (int i = kSwapAt + livehost::Runner::kFade; i < kSwapAt + livehost::Runner::kFade + 2 * kDelay; ++i)
            duringHold = std::max(duringHold, std::abs(heldOut[static_cast<std::size_t>(i)]));
        const bool held = duringHold == 0.0f && std::abs(heldOut.back() - 2.0f * kLevel) < 1e-6f;
        //  for one that adds a track: silent before, heard after
        const auto added = swapRender(block, one, &both, onSecond);
        if (added.out.getNumSamples() == 0) return 1;
        float before = 0.0f, after = 0.0f;
        for (int ch = 0; ch < 2; ++ch) {
            //  the old program, which encodes only the silent pair, plays until the fade out ends
            before = std::max(before, added.out.getMagnitude(ch, 0, kSwapAt + livehost::Runner::kFade));
            after = std::max(after, added.out.getMagnitude(ch, kSwapAt + 2 * livehost::Runner::kFade,
                                                           kLength - kSwapAt - 2 * livehost::Runner::kFade));
        }
        const bool heard = before == 0.0f && after > 1e-3f;
        const bool freed = added.keptThroughSwap && added.freedByTidy && SnapshotCensus::freedOnAudioThread() == 0;
        const bool ok = faded && held && heard && freed;
        failed += ok ? 0 : 1;
        std::printf(
            "the swap, block %4d: faded within %.3g, %s, the added track %s (peak %.4f after), the old program "
            "%s %s\n",
            block, off, held ? "silent through the delay" : "NOT silent through the delay",
            heard ? "heard only after" : "NOT as it should be", static_cast<double>(after),
            freed ? "freed by tidy, not in a block" : "NOT freed as it should be", ok ? "" : "  FAIL");
    }

    std::printf("a send's level, changed while it plays\n");
    for (const int block : {1, 64, 100, 1500}) {
        //  Catches: a level taken at once rather than on the grid, a ramp anchored to the block rather than to the
        //  grid, a step where a ramp should be, or a first block that ramps up from 1.
        const auto gains = sendGains(block);
        if (gains.size() != static_cast<std::size_t>(kRampLength)) return 1;
        double off = 0.0;
        for (int i = 0; i < kRampLength; ++i)
            off = std::max(off, static_cast<double>(std::abs(gains[static_cast<std::size_t>(i)] - expectedGain(i))));
        const bool ok = off <= 1e-5;
        failed += ok ? 0 : 1;
        std::printf("the send's level, block %4d: within %.3g of the grid's ramp %s\n", block, off, ok ? "" : "  FAIL");
    }

    for (const int block : {1, 64, 100, 1500}) {
        //  Catches: a program swapped in between two grid points that plays its defaults until the next one.
        constexpr int at = 48000;  // not a point of the grid
        const auto gains = sendGains(block, at, 0.5f);
        if (gains.size() != static_cast<std::size_t>(kRampLength)) return 1;
        double off = 0.0;
        for (int i = at + livehost::Runner::kFade + 1; i < at + 2 * livehost::Runner::kFade; ++i)
            off = std::max(off, static_cast<double>(std::abs(gains[static_cast<std::size_t>(i)] - 0.5f)));
        const bool ok = off <= 1e-4;
        failed += ok ? 0 : 1;
        std::printf("the send's level through a swap, block %4d: within %.3g of the new program's %s\n", block, off,
                    ok ? "" : "  FAIL");
    }

    std::printf("an insert's sidechain\n");
    for (const int block : {1, 64, 100, 1500}) {
        //  Catches: an Encoder's or an insert's sidechain left silent, fed from the track's own pair or another, or
        //  one heard by a slot that has none.
        float off = 0.0f, loud = 0.0f;
        for (const bool encoder : {false, true}) {
            const auto heard = sidechained(block, 1, encoder);
            const auto none = sidechained(block, -1, encoder);
            if (heard.size() != static_cast<std::size_t>(kLength) || none.size() != heard.size()) return 1;
            for (std::size_t i = 0; i < heard.size(); ++i) {
                off = std::max(off, std::abs(heard[i] - 3.0f * kLevel));
                loud = std::max(loud, std::abs(none[i]));
            }
        }
        const bool ok = off <= 1e-6f && loud == 0.0f;
        failed += ok ? 0 : 1;
        std::printf("the sidechains, block %4d: an Encoder's and an insert's their pair within %.3g, none silent %s\n",
                    block, static_cast<double>(off), ok ? "" : "  FAIL");
    }

    std::printf("the Decoder, wide\n");
    failed += wideDecoder();
    std::printf("the Decoder, remote-controlled\n");
    failed += remoteDecoder();

    std::printf("a source where it arrives\n");
    {
        //  Catches: a source read from a pair's start instead of its own channel, a mono input heard on one side
        //  only or with the channel beside it.
        const auto [l1, r1] = arrivedAt(2, 1);
        const auto [l2, r2] = arrivedAt(1, 2);
        const bool ok = std::abs(l1 - 0.3f) < 1e-6f && std::abs(r1 - 0.3f) < 1e-6f && std::abs(l2 - 0.2f) < 1e-6f &&
                        std::abs(r2 - 0.3f) < 1e-6f;
        failed += ok ? 0 : 1;
        std::printf("  mono from channel 3: %.2f %.2f; a pair from channel 2: %.2f %.2f %s\n", static_cast<double>(l1),
                    static_cast<double>(r1), static_cast<double>(l2), static_cast<double>(r2), ok ? "" : "  FAIL");
    }

    std::printf(failed == 0 ? "the runner plays both plans as the direct renders do, at every block size\n"
                            : "%d renders differ\n",
                failed);
    return failed == 0 ? 0 : 1;
}
