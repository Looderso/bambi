// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <juce_audio_processors/juce_audio_processors.h>

#include "bambi/mod/beatclock.hpp"
#include "bambi/mod/modulation.hpp"

/*  Walking a host's block on the control grid.
 *
 *  A block is not a unit of anything: hosts choose it, change it, and split it at events. The grid
 *  is fixed at 256 samples and pinned to the host's timeline, which is what makes a bounce identical
 *  to what was heard whatever block sizes the two used. Every plugin in the suite walks a block the
 *  same way, so it renders the same way too.
 *
 *  It also holds the rule about MIDI: a note belongs to the first control step at or after its
 *  sample. Where the host cuts blocks never moves a trigger.
 */
namespace bambi::host {

class ControlGrid {
public:
    /// Preparing to play. The next block steps at its first sample.
    void forget() noexcept {
        since_ = 0;
        now_ = true;
        carriedCount_ = 0;
        at_ = 0;
        beats_.forget();
    }

    /// A start or a locate: the grid is pinned to where the timeline is, not to wherever processing began; the step at the reset point is forced.
    void restartAt(std::int64_t timelineSample, int hop) noexcept {
        const auto h = static_cast<std::int64_t>(std::max(1, hop));
        since_ = static_cast<int>(((timelineSample % h) + h) % h);
        at_ = timelineSample;
        beats_.forget();
        now_ = true;
        carriedCount_ = 0;  // notes from before a jump belong to a timeline no longer playing
    }

    /// Step at the next segment whatever the grid says: a state was loaded under it.
    void forceStep() noexcept { now_ = true; }

    /// Where the grid sits, 0..hop-1. For a readout: it is otherwise invisible from outside.
    int phase() const noexcept { return since_; }

    /// The timeline sample the next walk starts at, counted on since the last start or locate. Never reached by a host's stale or jittered timeline between locates.
    std::int64_t position() const noexcept { return at_; }

    /// The host's song position at the start of the block about to be walked. Stopped, it holds: it runs at no tempo while the grid counts on.
    void hear(double ppq, double bpm, bool playing, double sampleRate) noexcept {
        beats_.observe(at_, ppq, playing ? bpm : 0.0, sampleRate);
    }

    /// Quarter notes at the step being taken, from inside `onStep`: a function of the step's sample, never of where the host began the block.
    double ppqNow() const noexcept { return beats_.ppqAt(at_); }

    /*  Walk `numSamples`, calling `onStep(pos)` on each control step and `onSegment(pos, frames)`
     *  for the audio between them. Real-time safe: the callables are template parameters, so there
     *  is no std::function and nothing to allocate.
     *
     *  Notes are handed to `mod` at the step they belong to, before that step runs. A note after a
     *  block's last step belongs to the next block's first, and is carried there: where the host
     *  cuts its blocks never moves a trigger, and never loses one. */
    template <class Step, class Segment>
    void walk(int numSamples, int hop, const juce::MidiBuffer& midi, ModulationEngine& mod, Step&& onStep,
              Segment&& onSegment) {
        const auto n = static_cast<std::size_t>(std::max(0, numSamples));
        std::size_t pos = 0;
        auto next = midi.cbegin();
        const auto end = midi.cend();
        while (pos < n) {
            if (since_ == 0 || now_) {
                //  carried notes from the last block first, then this block's up to here
                playCarried(mod);
                for (; next != end && (*next).samplePosition <= static_cast<int>(pos); ++next)
                    play(mod, (*next).getMessage());
                onStep(pos);
                now_ = false;
            }
            const auto m = std::min<std::size_t>(n - pos, static_cast<std::size_t>(hop - since_));
            onSegment(pos, m);
            pos += m;
            at_ += static_cast<std::int64_t>(m);
            since_ = static_cast<int>((static_cast<std::size_t>(since_) + m) % static_cast<std::size_t>(hop));
        }
        for (; next != end; ++next)  // after this block's last step: the next block's first
            carry((*next).getMessage());
    }

    /// A block that is not walked -- the layout changed under it, the host sent more than it said it would -- still had notes in it, and they still belong to the next step.
    void carryAll(const juce::MidiBuffer& midi) noexcept {
        for (const auto metadata : midi) carry(metadata.getMessage());
    }

    /// A block that is not walked still passed on the timeline: the grid moves on by it, so its steps and positions stay where a walked block would have left them. Its notes are carried.
    void pass(int numSamples, int hop, const juce::MidiBuffer& midi) noexcept {
        carryAll(midi);
        const auto n = std::max(0, numSamples);
        at_ += n;
        since_ = (since_ + n % std::max(1, hop)) % std::max(1, hop);
    }

    /// Notes the carry-over could not hold, ever. For a readout.
    int notesDropped() const noexcept { return dropped_; }

private:
    enum class NoteKind { None, Note, AllOff };
    static NoteKind toNote(const juce::MidiMessage& message, NoteEvent& event) noexcept {
        if (message.isNoteOn()) {
            event = {message.getChannel(), message.getNoteNumber(), message.getFloatVelocity(), true};
            return NoteKind::Note;
        }
        if (message.isNoteOff())  // including a note-on at velocity 0
        {
            event = {message.getChannel(), message.getNoteNumber(), 0.0f, false};
            return NoteKind::Note;
        }
        return message.isAllNotesOff() || message.isAllSoundOff() ? NoteKind::AllOff : NoteKind::None;
    }

    static void play(ModulationEngine& mod, const juce::MidiMessage& message) noexcept {
        NoteEvent event;
        switch (toNote(message, event)) {
            case NoteKind::Note: mod.note(event); break;
            case NoteKind::AllOff: mod.allNotesOff(); break;
            case NoteKind::None: break;
        }
    }

    void carry(const juce::MidiMessage& message) noexcept {
        NoteEvent event;
        const auto kind = toNote(message, event);
        if (kind == NoteKind::None) return;
        if (carriedCount_ >= static_cast<int>(carried_.size())) {
            ++dropped_;
            return;
        }
        carried_[static_cast<std::size_t>(carriedCount_++)] = {event, kind == NoteKind::AllOff};
    }

    void playCarried(ModulationEngine& mod) noexcept {
        for (int i = 0; i < carriedCount_; ++i) {
            const auto& c = carried_[static_cast<std::size_t>(i)];
            if (c.allOff)
                mod.allNotesOff();
            else
                mod.note(c.event);
        }
        carriedCount_ = 0;
    }

    struct Carried {
        NoteEvent event;
        bool allOff{false};
    };
    std::array<Carried, 256> carried_{};
    int carriedCount_{0};
    int dropped_{0};

    BeatClock beats_;
    std::int64_t at_{0};  ///< the timeline sample `pos` 0 of the next walk is
    int since_{0};        ///< samples since the last step
    bool now_{true};      ///< the next segment starts a step whatever the grid says
};

}  // namespace bambi::host
