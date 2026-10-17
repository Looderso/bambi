// SPDX-License-Identifier: GPL-3.0-or-later
#include "Runner.h"

#include <algorithm>

#include "bambi/math/denormals.hpp"

namespace bambi::livehost {

namespace {

//  `width` channels of the input row from `from` onto two of `scratch` from `first`, faded as the sources are: a
//  pair as it is, one channel on both sides.
void arrive(juce::AudioBuffer<float>& scratch, int first, const juce::AudioBuffer<float>& in, int from, int width,
            int inStart, int frames, float fade0, float fade1) noexcept {
    for (int ch = 0; ch < 2 && first + ch < scratch.getNumChannels(); ++ch) {
        const int source = from + (width == 2 ? ch : 0);
        if (source < in.getNumChannels())
            scratch.copyFromWithRamp(first + ch, 0, in.getReadPointer(source, inStart), frames, fade0, fade1);
    }
}

//  A slot's sidechain, the two channels after its main input's from `first`; silent, as the scratch was cleared,
//  for a step that names none.
void sidechain(const live::Op& op, juce::AudioBuffer<float>& scratch, int first, const juce::AudioBuffer<float>& in,
               int inStart, int frames, float fade0, float fade1) noexcept {
    if (op.sidechain >= 0)
        arrive(scratch, first, in, op.sidechain, op.sidechainWidth, inStart, frames, fade0, fade1);
}

}  // namespace

std::vector<Runner::Route> Runner::mainOf(const std::vector<Route>& routes) {
    std::vector<Route> out;
    for (const Route& r : routes)
        if (r.main) out.push_back(r);
    return out;
}

std::unique_ptr<Runner::Program> Runner::build(const live::Compiled& compiled,
                                               const std::vector<juce::AudioProcessor*>& slots, int levels,
                                               juce::String& error, Keep keep, std::vector<Route> routes) {
    //  every index a step names, checked here, so that process() can trust them
    const auto buffer = [&](int b) { return b >= 0 && b < compiled.buffers; };
    for (const live::Op& op : compiled.ops) {
        const bool needsSlot =
            op.step == live::Step::Encode || op.step == live::Step::Process || op.step == live::Step::Decode;
        if (needsSlot && (op.slot < 0 || op.slot >= static_cast<int>(slots.size()) ||
                          slots[static_cast<std::size_t>(op.slot)] == nullptr)) {
            error = "the plan names slot " + juce::String(op.slot) + ", which has no instance";
            return nullptr;
        }
        const bool readsBuffer = op.step == live::Step::Add || op.step == live::Step::Decode;
        const bool writesBuffer = op.step != live::Step::Decode;
        if ((readsBuffer && !buffer(op.from)) || (writesBuffer && !buffer(op.to)) ||
            (op.step == live::Step::Encode && (op.from < 0 || op.from + op.width > setup_.inputChannels)) ||
            op.sidechain + op.sidechainWidth > setup_.inputChannels) {
            error = "a step reads or writes a buffer or input the plan does not have";
            return nullptr;
        }
        if (op.gain >= levels) {
            error = "a send reads level " + juce::String(op.gain) + " of " + juce::String(levels);
            return nullptr;
        }
    }
    int decoderWidth = 0;
    for (const live::Op& op : compiled.ops)
        if (op.step == live::Step::Decode)
            decoderWidth = slots[static_cast<std::size_t>(op.slot)]->getTotalNumOutputChannels();
    for (const Route& r : routes) {
        const bool from = r.main ? r.from == 0 || r.from == 1 : r.from >= 0 && r.from < decoderWidth;
        const bool late = r.delay >= 0 && (!r.main || r.delay <= setup_.maxMainDelay);
        const bool heard = !r.main || (setup_.mainInput >= 0 && setup_.mainInput + 1 < setup_.inputChannels);
        if (!from || !late || !heard || r.to < 0 || r.to >= setup_.outputChannels) {
            error = "a route reads or writes a channel the host does not have, or is later than Main may be";
            return nullptr;
        }
    }
    auto p = std::make_unique<Program>();
    p->compiled = compiled;
    p->slots = slots;
    p->keep = std::move(keep);
    p->buffers.assign(static_cast<std::size_t>(compiled.buffers), juce::AudioBuffer<float>(field_, setup_.maxBlock));
    for (juce::AudioProcessor* plugin : slots) {
        const int width =
            plugin == nullptr ? 0 : std::max(plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels());
        p->scratch.emplace_back(std::max(width, field_), setup_.maxBlock);
    }
    for (const live::Op& op : compiled.ops) {
        Line l;
        if (op.step == live::Step::Add && op.delay > 0) {
            p->longest = std::max(p->longest, op.delay);
            l.length = op.delay;
            l.memory.setSize(field_, op.delay);
            l.memory.clear();
        }
        p->lines.push_back(std::move(l));
    }
    for (const Route& r : routes) {
        Line l;
        if (!r.main) p->longest = std::max(p->longest, r.delay);
        if (!r.main && r.delay > 0) {
            l.length = r.delay;
            l.memory.setSize(1, r.delay);
            l.memory.clear();
        }
        p->routeLines.push_back(std::move(l));
    }
    p->routes = std::move(routes);
    p->levelCount = std::max(levels, 0);
    p->levels = std::make_unique<Program::Level[]>(static_cast<std::size_t>(p->levelCount));
    return p;
}

bool Runner::prepare(const live::Compiled& compiled, const std::vector<juce::AudioProcessor*>& slots, int levels,
                     const Setup& setup, juce::String& error, Keep keep, std::vector<Route> routes) {
    setup_ = setup;
    field_ = (setup.order + 1) * (setup.order + 1);
    auto p = build(compiled, slots, levels, error, std::move(keep), std::move(routes));
    if (p == nullptr) return false;
    for (juce::AudioProcessor* plugin : slots) ready(plugin);  // no audio runs yet
    latest_ = p.get();
    handoff_.begin(std::move(p));
    midi_.ensureSize(256);
    playhead_.set({}, setup.rate);
    sample_ = 0;
    fade_ = Fade::None;
    mainLines_.setSize(2, std::max(1, setup.maxMainDelay + setup.maxBlock));
    mainLines_.clear();
    mainAt_ = 0;
    return true;
}

bool Runner::swap(const live::Compiled& compiled, const std::vector<juce::AudioProcessor*>& slots, int levels,
                  juce::String& error, Keep keep, std::vector<Route> routes) {
    auto p = build(compiled, slots, levels, error, std::move(keep), std::move(routes));
    if (p == nullptr) return false;
    //  sticky while a program is still waiting: what the audio thread adopts must fade Main if any program posted since
    //  it last adopted plays Main otherwise; published to it by the post below
    const bool differs = latest_ == nullptr || mainOf(latest_->routes) != mainOf(p->routes);
    mainMoves_.store(differs || (handoff_.pending() && mainMoves_.load(std::memory_order_relaxed)),
                     std::memory_order_release);
    latest_ = p.get();
    (void)handoff_.post(std::move(p));  // one posted and never taken goes here, on this thread
    return true;
}

void Runner::ready(juce::AudioProcessor* plugin) {
    if (plugin == nullptr) return;
    plugin->setNonRealtime(false);
    plugin->setPlayHead(&playhead_);
}

void Runner::setLevel(int entry, float level) noexcept {
    if (latest_ != nullptr && entry >= 0 && entry < latest_->levelCount)
        latest_->levels[static_cast<std::size_t>(entry)].target.store(level, std::memory_order_relaxed);
}

//  A plugin over `scratch`, already sized to the piece: its inputs as the caller filled them, its outputs left there.
void Runner::run(const Program& p, int slot, juce::AudioBuffer<float>& scratch) noexcept {
    midi_.clear();
    p.slots[static_cast<std::size_t>(slot)]->processBlock(scratch, midi_);
}

void Runner::process(const juce::AudioBuffer<float>& loop, juce::AudioBuffer<float>& output, int frames) noexcept {
    //  the runner's own sums and delays, as flushed as the plugins' processing
    ScopedFlushDenormals noDenormals;
    const SnapshotCensus::InAudioBlock inBlock;
    output.clear(0, frames);
    if (handoff_.current() == nullptr) return;
    for (int done = 0; done < frames;) {
        if (fade_ == Fade::None && handoff_.pending()) {
            fade_ = Fade::Out;
            faded_ = 0;
            mainFading_ = mainMoves_.load(std::memory_order_acquire);
        }
        //  a piece ends where a phase of the fade does, so the swap falls between two pieces, and at a grid point,
        //  where the sends take their levels
        int n = std::min({frames - done, setup_.maxBlock, kLevelGrid - static_cast<int>(sample_ % kLevelGrid)});
        const int length = fade_ == Fade::Hold ? hold_ : kFade;
        if (fade_ != Fade::None) n = std::min(n, length - faded_);
        //  the fade's gains at the piece's first sample, and just after its last: the field's, and Main's
        float g0 = 1.0f, g1 = 1.0f, m0 = 1.0f, m1 = 1.0f;
        const auto ramp = [&](int at) { return static_cast<float>(at) / static_cast<float>(kFade); };
        if (fade_ == Fade::Out || fade_ == Fade::MainOut) {
            m0 = 1.0f - ramp(faded_);
            m1 = 1.0f - ramp(faded_ + n);
            g0 = fade_ == Fade::Out ? m0 : 0.0f;
            g1 = fade_ == Fade::Out ? m1 : 0.0f;
        } else if (fade_ == Fade::Hold) {
            g0 = g1 = m0 = m1 = 0.0f;
        } else if (fade_ == Fade::In) {
            g0 = m0 = ramp(faded_);
            g1 = m1 = ramp(faded_ + n);
        }
        piece(*handoff_.current(), loop, done, output, done, n, g0, g1);
        if (fade_ != Fade::None) output.applyGainRamp(done, n, g0, g1);
        const bool mainFades = fade_ != Fade::None && mainFading_;
        main(*handoff_.current(), loop, done, output, done, n, mainFades ? m0 : 1.0f, mainFades ? m1 : 1.0f);
        if (fade_ != Fade::None) {
            faded_ += n;
            if (faded_ == length) {
                faded_ = 0;
                if (fade_ == Fade::Out && handoff_.current()->longest > 0) {
                    //  silence through the old program's longest delay, so the lines it drops hold nothing
                    fade_ = Fade::Hold;
                    hold_ = handoff_.current()->longest;
                } else if ((fade_ == Fade::Out || fade_ == Fade::Hold) && !mainFading_ &&
                           mainMoves_.load(std::memory_order_acquire)) {
                    //  a program posted while the field faded plays Main otherwise: Main fades out too, first
                    fade_ = Fade::MainOut;
                    mainFading_ = true;
                } else if (fade_ == Fade::Out || fade_ == Fade::Hold || fade_ == Fade::MainOut) {
                    //  the new program, faded in -- or, with no room to retire the old one, the old one again
                    if (const Program* next = handoff_.adopt())
                        for (int i = 0; i < next->levelCount; ++i) {
                            const auto& l = next->levels[static_cast<std::size_t>(i)];
                            l.from = l.to = l.target.load(std::memory_order_relaxed);
                            l.begun = true;
                        }
                    fade_ = Fade::In;
                } else {
                    fade_ = Fade::None;
                }
            }
        }
        done += n;
    }
}

void Runner::piece(const Program& p, const juce::AudioBuffer<float>& loop, int loopStart,
                   juce::AudioBuffer<float>& output, int outStart, int frames, float fade0, float fade1) noexcept {
    if (clock_ != nullptr) playhead_.set(clock_->reading(sample_), setup_.rate);
    const int offset = static_cast<int>(sample_ % kLevelGrid);
    if (offset == 0)
        for (int i = 0; i < p.levelCount; ++i) {
            const auto& l = p.levels[static_cast<std::size_t>(i)];
            const float before = l.to;  // where the last interval's ramp ended
            l.to = l.target.load(std::memory_order_relaxed);
            l.from = l.begun ? before : l.to;
            l.begun = true;
        }
    for (std::size_t k = 0; k < p.compiled.ops.size(); ++k) {
        const live::Op& op = p.compiled.ops[k];
        switch (op.step) {
            case live::Step::Encode: {
                auto& s = p.scratch[static_cast<std::size_t>(op.slot)];
                s.setSize(s.getNumChannels(), frames, false, false, true);
                s.clear(0, frames);
                //  faded with the output through a swap, so no insert or line kept across it hears a step
                arrive(s, 0, loop, op.from, op.width, loopStart, frames, fade0, fade1);
                sidechain(op, s, 2, loop, loopStart, frames, fade0, fade1);
                run(p, op.slot, s);
                for (int ch = 0; ch < field_; ++ch)
                    p.buffers[static_cast<std::size_t>(op.to)].copyFrom(ch, 0, s, ch, 0, frames);
                break;
            }
            case live::Step::Process: {
                auto& s = p.scratch[static_cast<std::size_t>(op.slot)];
                auto& b = p.buffers[static_cast<std::size_t>(op.to)];
                s.setSize(s.getNumChannels(), frames, false, false, true);
                s.clear(0, frames);
                for (int ch = 0; ch < field_; ++ch) s.copyFrom(ch, 0, b, ch, 0, frames);
                //  its sidechain, the channels after the field's: a source's own pair, faded as the sources are
                sidechain(op, s, field_, loop, loopStart, frames, fade0, fade1);
                run(p, op.slot, s);
                for (int ch = 0; ch < field_; ++ch) b.copyFrom(ch, 0, s, ch, 0, frames);
                break;
            }
            case live::Step::Clear: p.buffers[static_cast<std::size_t>(op.to)].clear(0, frames); break;
            case live::Step::Add: {
                float g0 = 1.0f, g1 = 1.0f;  // the gain at the piece's first sample, and just after its last
                if (op.gain >= 0) {
                    const auto& l = p.levels[static_cast<std::size_t>(op.gain)];
                    g0 = gainAt(l, offset);
                    g1 = gainAt(l, offset + frames);
                }
                auto& to = p.buffers[static_cast<std::size_t>(op.to)];
                const auto& from = p.buffers[static_cast<std::size_t>(op.from)];
                Line& line = p.lines[k];
                if (line.length == 0) {
                    for (int ch = 0; ch < field_; ++ch)
                        to.addFromWithRamp(ch, 0, from.getReadPointer(ch), frames, g0, g1);
                    break;
                }
                const float step = (g1 - g0) / static_cast<float>(frames);
                for (int ch = 0; ch < field_; ++ch) {
                    float* mem = line.memory.getWritePointer(ch);
                    const float* in = from.getReadPointer(ch);
                    float* dst = to.getWritePointer(ch);
                    int at = line.at;
                    for (int i = 0; i < frames; ++i) {
                        const float delayed = mem[at];
                        mem[at] = in[i];
                        dst[i] += (g0 + step * static_cast<float>(i)) * delayed;
                        if (++at == line.length) at = 0;
                    }
                }
                line.at = static_cast<int>((line.at + frames) % line.length);
                break;
            }
            case live::Step::Decode: {
                auto& s = p.scratch[static_cast<std::size_t>(op.slot)];
                const auto& b = p.buffers[static_cast<std::size_t>(op.from)];
                s.setSize(s.getNumChannels(), frames, false, false, true);
                s.clear(0, frames);
                for (int ch = 0; ch < field_; ++ch) s.copyFrom(ch, 0, b, ch, 0, frames);
                run(p, op.slot, s);
                if (p.routes.empty()) {
                    const int channels =
                        std::min({output.getNumChannels(), setup_.outputChannels, s.getNumChannels()});
                    for (int ch = 0; ch < channels; ++ch) output.copyFrom(ch, outStart, s, ch, 0, frames);
                    break;
                }
                for (std::size_t r = 0; r < p.routes.size(); ++r) {
                    const Route& route = p.routes[r];
                    if (route.main || route.to >= output.getNumChannels()) continue;
                    Line& line = p.routeLines[r];
                    if (line.length == 0) {
                        output.addFrom(route.to, outStart, s, route.from, 0, frames);
                        continue;
                    }
                    float* mem = line.memory.getWritePointer(0);
                    const float* in = s.getReadPointer(route.from);
                    float* dst = output.getWritePointer(route.to, outStart);
                    int at = line.at;
                    for (int i = 0; i < frames; ++i) {
                        dst[i] += mem[at];
                        mem[at] = in[i];
                        if (++at == line.length) at = 0;
                    }
                    line.at = at;
                }
                break;
            }
        }
    }
    sample_ += frames;
}

void Runner::main(const Program& p, const juce::AudioBuffer<float>& in, int inStart, juce::AudioBuffer<float>& output,
                  int outStart, int frames, float g0, float g1) noexcept {
    const int capacity = mainLines_.getNumSamples();
    if (setup_.mainInput < 0 || setup_.mainInput + 1 >= in.getNumChannels() || capacity < frames) return;
    for (int ch = 0; ch < 2; ++ch) {
        const float* src = in.getReadPointer(setup_.mainInput + ch, inStart);
        float* line = mainLines_.getWritePointer(ch);
        for (int i = 0, at = mainAt_; i < frames; ++i, at = at + 1 == capacity ? 0 : at + 1) line[at] = src[i];
    }
    const float step = (g1 - g0) / static_cast<float>(frames);
    for (const Route& r : p.routes) {
        if (!r.main || r.to >= output.getNumChannels()) continue;
        const float* line = mainLines_.getReadPointer(r.from);
        float* dst = output.getWritePointer(r.to, outStart);
        int at = mainAt_ - r.delay;
        if (at < 0) at += capacity;
        for (int i = 0; i < frames; ++i) {
            dst[i] += (g0 + step * static_cast<float>(i)) * line[at];
            if (++at == capacity) at = 0;
        }
    }
    mainAt_ = (mainAt_ + frames) % capacity;
}

}  // namespace bambi::livehost
