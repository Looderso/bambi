// SPDX-License-Identifier: GPL-3.0-or-later
#include "Runner.h"

#include <algorithm>

#include "bambi/math/denormals.hpp"

namespace bambi::livehost {

std::unique_ptr<Runner::Program> Runner::build(const live::Compiled& compiled,
                                               const std::vector<juce::AudioProcessor*>& slots, int levels,
                                               juce::String& error, Keep keep) {
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
            (op.step == live::Step::Encode && (op.from < 0 || 2 * op.from + 1 >= setup_.loopChannels))) {
            error = "a step reads or writes a buffer or pair the plan does not have";
            return nullptr;
        }
        if (op.gain >= levels) {
            error = "a send reads level " + juce::String(op.gain) + " of " + juce::String(levels);
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
            l.length = op.delay;
            l.memory.setSize(field_, op.delay);
            l.memory.clear();
        }
        p->lines.push_back(std::move(l));
    }
    p->levelCount = std::max(levels, 0);
    p->levels = std::make_unique<std::atomic<float>[]>(static_cast<std::size_t>(p->levelCount));
    for (int i = 0; i < p->levelCount; ++i)
        p->levels[static_cast<std::size_t>(i)].store(1.0f, std::memory_order_relaxed);
    for (juce::AudioProcessor* plugin : slots)
        if (plugin != nullptr) {
            plugin->setNonRealtime(false);
            plugin->setPlayHead(&playhead_);
        }
    return p;
}

bool Runner::prepare(const live::Compiled& compiled, const std::vector<juce::AudioProcessor*>& slots, int levels,
                     const Setup& setup, juce::String& error, Keep keep) {
    setup_ = setup;
    field_ = (setup.order + 1) * (setup.order + 1);
    auto p = build(compiled, slots, levels, error, std::move(keep));
    if (p == nullptr) return false;
    latest_ = p.get();
    handoff_.begin(std::move(p));
    midi_.ensureSize(256);
    playhead_.set({}, setup.rate);
    sample_ = 0;
    fade_ = Fade::None;
    return true;
}

bool Runner::swap(const live::Compiled& compiled, const std::vector<juce::AudioProcessor*>& slots, int levels,
                  juce::String& error, Keep keep) {
    auto p = build(compiled, slots, levels, error, std::move(keep));
    if (p == nullptr) return false;
    latest_ = p.get();
    (void)handoff_.post(std::move(p));  // one posted and never taken goes here, on this thread
    return true;
}

void Runner::setLevel(int entry, float level) noexcept {
    if (latest_ != nullptr && entry >= 0 && entry < latest_->levelCount)
        latest_->levels[static_cast<std::size_t>(entry)].store(level, std::memory_order_relaxed);
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
        }
        //  a piece ends where a fade does, so the swap falls between two pieces
        int n = std::min(frames - done, setup_.maxBlock);
        if (fade_ != Fade::None) n = std::min(n, kFade - faded_);
        piece(*handoff_.current(), loop, done, output, done, n);
        if (fade_ != Fade::None) {
            const float from = static_cast<float>(faded_) / kFade, to = static_cast<float>(faded_ + n) / kFade;
            if (fade_ == Fade::Out)
                output.applyGainRamp(done, n, 1.0f - from, 1.0f - to);
            else
                output.applyGainRamp(done, n, from, to);
            faded_ += n;
            if (faded_ == kFade) {
                faded_ = 0;
                if (fade_ == Fade::Out) {
                    //  the new program, faded in -- or, with no room to retire the old one, the old one again
                    (void)handoff_.adopt();
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
                   juce::AudioBuffer<float>& output, int outStart, int frames) noexcept {
    if (clock_ != nullptr) playhead_.set(clock_->reading(sample_), setup_.rate);
    for (std::size_t k = 0; k < p.compiled.ops.size(); ++k) {
        const live::Op& op = p.compiled.ops[k];
        switch (op.step) {
            case live::Step::Encode: {
                auto& s = p.scratch[static_cast<std::size_t>(op.slot)];
                s.setSize(s.getNumChannels(), frames, false, false, true);
                s.clear(0, frames);  // the sidechain stays silent
                for (int ch = 0; ch < 2; ++ch)
                    if (2 * op.from + ch < loop.getNumChannels())
                        s.copyFrom(ch, 0, loop, 2 * op.from + ch, loopStart, frames);
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
                run(p, op.slot, s);
                for (int ch = 0; ch < field_; ++ch) b.copyFrom(ch, 0, s, ch, 0, frames);
                break;
            }
            case live::Step::Clear: p.buffers[static_cast<std::size_t>(op.to)].clear(0, frames); break;
            case live::Step::Add: {
                const float level =
                    op.gain < 0 ? 1.0f : p.levels[static_cast<std::size_t>(op.gain)].load(std::memory_order_relaxed);
                auto& to = p.buffers[static_cast<std::size_t>(op.to)];
                const auto& from = p.buffers[static_cast<std::size_t>(op.from)];
                Line& line = p.lines[k];
                if (line.length == 0) {
                    for (int ch = 0; ch < field_; ++ch) to.addFrom(ch, 0, from, ch, 0, frames, level);
                    break;
                }
                for (int ch = 0; ch < field_; ++ch) {
                    float* mem = line.memory.getWritePointer(ch);
                    const float* in = from.getReadPointer(ch);
                    float* dst = to.getWritePointer(ch);
                    int at = line.at;
                    for (int i = 0; i < frames; ++i) {
                        const float delayed = mem[at];
                        mem[at] = in[i];
                        dst[i] += level * delayed;
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
                const int channels = std::min({output.getNumChannels(), setup_.outputChannels, s.getNumChannels()});
                for (int ch = 0; ch < channels; ++ch) output.copyFrom(ch, outStart, s, ch, 0, frames);
                break;
            }
        }
    }
    sample_ += frames;
}

}  // namespace bambi::livehost
