// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <optional>

/*  MIDI on its way to the plugins the live host plays: short messages placed on the host's samples, held until the
 *  piece of audio they fall in, and how far Live's MIDI runs ahead of its audio, as the host measures it.
 *
 *  Fixed capacities, so the audio thread can own them: nothing allocates. Past capacity an event is dropped.
 */
namespace bambi::live {

/// A short MIDI message -- a note, a controller -- at a host sample.
struct MidiEvent {
    std::int64_t sample{0};
    std::array<std::uint8_t, 3> bytes{};
    std::uint8_t size{0};
};

class MidiPending {
public:
    static constexpr int kCapacity = 1024;

    /// False when full: the event is dropped.
    bool add(const MidiEvent& e) noexcept;

    /// Every event before `end` into `out`, in sample order, at most `room` of them, the rest kept for later;
    /// returns how many.
    int take(std::int64_t end, MidiEvent* out, int room) noexcept;

    int size() const noexcept { return count_; }
    void clear() noexcept { count_ = 0; }

private:
    std::array<MidiEvent, kCapacity> events_{};
    int count_{0};
};

/// The last measurements of how far Live's MIDI arrives ahead of its audio: a note and the sound it sets off.
class MidiLead {
public:
    static constexpr int kKept = 16;

    void add(double ms) noexcept;
    int count() const noexcept { return count_; }
    /// The middle of those kept; 0 before any.
    double median() const noexcept;
    /// Half the range of the middle half of them: how far they agree.
    double spread() const noexcept;

private:
    std::array<double, kKept> kept_{};
    int count_{0}, next_{0};
};

/// One measurement of the lead at a time, from Live's notes and the sound on Live's Main: a note-on's arrival paired
/// with the onset nearest after it -- or, when the sound came first, the one just before -- within `window`. An onset
/// is a rise above `threshold` after `quiet` of silence, so a test wants a click on the beats a MIDI track plays,
/// and little else. Audio thread; arithmetic only.
class LeadMeter {
public:
    explicit LeadMeter(double rate, float threshold = 0.03f) noexcept
        : threshold_(threshold), quiet_(static_cast<std::int64_t>(0.010 * rate)),
          window_(static_cast<std::int64_t>(0.300 * rate)) {}

    /// A note-on from Live arrived, at the host's sample `at`.
    void note(std::int64_t at) noexcept;
    /// Live's Main, `frames` of it from the host's sample `start`; either side may be null.
    void listen(const float* left, const float* right, int frames, std::int64_t start) noexcept;
    /// The last pair's lead -- the onset's sample less the note's -- once; none since.
    std::optional<std::int64_t> took() noexcept;

private:
    float threshold_;
    std::int64_t quiet_, window_;
    std::int64_t silent_{0};  ///< samples below the threshold, up to now
    std::optional<std::int64_t> armed_, onset_, result_;
};

}  // namespace bambi::live
