// SPDX-License-Identifier: GPL-3.0-or-later
#include "Runner.h"

#include <algorithm>

#include "bambi/math/denormals.hpp"

namespace bambi::livehost {

bool Runner::prepare(const live::Compiled& compiled, const std::vector<juce::AudioProcessor*>& slots, int levels,
                     const Setup& setup, juce::String& error) {
    //  every index a step names, checked here, so that process() can trust them
    const auto buffer = [&](int b) { return b >= 0 && b < compiled.buffers; };
    for (const live::Op& op : compiled.ops) {
        const bool needsSlot =
            op.step == live::Step::Encode || op.step == live::Step::Process || op.step == live::Step::Decode;
        if (needsSlot && (op.slot < 0 || op.slot >= static_cast<int>(slots.size()) ||
                          slots[static_cast<std::size_t>(op.slot)] == nullptr)) {
            error = "the plan names slot " + juce::String(op.slot) + ", which has no instance";
            return false;
        }
        const bool readsBuffer = op.step == live::Step::Add || op.step == live::Step::Decode;
        const bool writesBuffer = op.step != live::Step::Decode;
        if ((readsBuffer && !buffer(op.from)) || (writesBuffer && !buffer(op.to)) ||
            (op.step == live::Step::Encode && (op.from < 0 || 2 * op.from + 1 >= setup.loopChannels))) {
            error = "a step reads or writes a buffer or pair the plan does not have";
            return false;
        }
        if (op.gain >= levels) {
            error = "a send reads level " + juce::String(op.gain) + " of " + juce::String(levels);
            return false;
        }
    }
    compiled_ = compiled;
    slots_ = slots;
    setup_ = setup;
    field_ = (setup.order + 1) * (setup.order + 1);

    buffers_.assign(static_cast<std::size_t>(compiled.buffers), juce::AudioBuffer<float>(field_, setup.maxBlock));
    scratch_.clear();
    for (juce::AudioProcessor* p : slots) {
        const int width = p == nullptr ? 0 : std::max(p->getTotalNumInputChannels(), p->getTotalNumOutputChannels());
        scratch_.emplace_back(std::max(width, field_), setup.maxBlock);
    }
    lines_.clear();
    for (const live::Op& op : compiled.ops) {
        Line l;
        if (op.step == live::Step::Add && op.delay > 0) {
            l.length = op.delay;
            l.memory.setSize(field_, op.delay);
            l.memory.clear();
        }
        lines_.push_back(std::move(l));
    }
    levelCount_ = std::max(levels, 0);
    levels_ = std::make_unique<std::atomic<float>[]>(static_cast<std::size_t>(levelCount_));
    for (int i = 0; i < levelCount_; ++i) levels_[static_cast<std::size_t>(i)].store(1.0f, std::memory_order_relaxed);
    midi_.ensureSize(256);
    for (juce::AudioProcessor* p : slots)
        if (p != nullptr) {
            p->setNonRealtime(false);
            p->setPlayHead(&playhead_);
        }
    playhead_.set({}, setup.rate);
    sample_ = 0;
    return true;
}

//  A plugin over `scratch`, already sized to the piece: its inputs as the caller filled them, its outputs left there.
void Runner::run(int slot, juce::AudioBuffer<float>& scratch) noexcept {
    midi_.clear();
    slots_[static_cast<std::size_t>(slot)]->processBlock(scratch, midi_);
}

void Runner::process(const juce::AudioBuffer<float>& loop, juce::AudioBuffer<float>& output, int frames) noexcept {
    //  the runner's own sums and delays, as flushed as the plugins' processing
    ScopedFlushDenormals noDenormals;
    output.clear(0, frames);
    for (int done = 0; done < frames;) {
        const int n = std::min(frames - done, setup_.maxBlock);
        piece(loop, done, output, done, n);
        done += n;
    }
}

void Runner::piece(const juce::AudioBuffer<float>& loop, int loopStart, juce::AudioBuffer<float>& output, int outStart,
                   int frames) noexcept {
    if (clock_ != nullptr) playhead_.set(clock_->reading(sample_), setup_.rate);
    for (std::size_t k = 0; k < compiled_.ops.size(); ++k) {
        const live::Op& op = compiled_.ops[k];
        switch (op.step) {
            case live::Step::Encode: {
                auto& s = scratch_[static_cast<std::size_t>(op.slot)];
                s.setSize(s.getNumChannels(), frames, false, false, true);
                s.clear(0, frames);  // the sidechain stays silent
                for (int ch = 0; ch < 2; ++ch)
                    if (2 * op.from + ch < loop.getNumChannels())
                        s.copyFrom(ch, 0, loop, 2 * op.from + ch, loopStart, frames);
                run(op.slot, s);
                for (int ch = 0; ch < field_; ++ch)
                    buffers_[static_cast<std::size_t>(op.to)].copyFrom(ch, 0, s, ch, 0, frames);
                break;
            }
            case live::Step::Process: {
                auto& s = scratch_[static_cast<std::size_t>(op.slot)];
                auto& b = buffers_[static_cast<std::size_t>(op.to)];
                s.setSize(s.getNumChannels(), frames, false, false, true);
                s.clear(0, frames);
                for (int ch = 0; ch < field_; ++ch) s.copyFrom(ch, 0, b, ch, 0, frames);
                run(op.slot, s);
                for (int ch = 0; ch < field_; ++ch) b.copyFrom(ch, 0, s, ch, 0, frames);
                break;
            }
            case live::Step::Clear: buffers_[static_cast<std::size_t>(op.to)].clear(0, frames); break;
            case live::Step::Add: {
                const float level =
                    op.gain < 0 ? 1.0f : levels_[static_cast<std::size_t>(op.gain)].load(std::memory_order_relaxed);
                auto& to = buffers_[static_cast<std::size_t>(op.to)];
                const auto& from = buffers_[static_cast<std::size_t>(op.from)];
                Line& line = lines_[k];
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
                auto& s = scratch_[static_cast<std::size_t>(op.slot)];
                const auto& b = buffers_[static_cast<std::size_t>(op.from)];
                s.setSize(s.getNumChannels(), frames, false, false, true);
                s.clear(0, frames);
                for (int ch = 0; ch < field_; ++ch) s.copyFrom(ch, 0, b, ch, 0, frames);
                run(op.slot, s);
                const int channels = std::min({output.getNumChannels(), setup_.outputChannels, s.getNumChannels()});
                for (int ch = 0; ch < channels; ++ch) output.copyFrom(ch, outStart, s, ch, 0, frames);
                break;
            }
        }
    }
    sample_ += frames;
}

}  // namespace bambi::livehost
