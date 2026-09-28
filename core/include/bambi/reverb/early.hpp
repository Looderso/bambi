// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <memory>
#include <vector>

#include "bambi/dsp/biquad.hpp"
#include "bambi/dsp/ramp.hpp"
#include "bambi/reverb/images.hpp"

/*  Reverb's early reflections, on the bus.
 *
 *  Reverb never sees a source, only the bus. The bus is read at twelve virtual sources, each with an
 *  order-3 max-rE beam; each is delayed; and every image of every virtual source is a tap on its delay
 *  line -- filtered by how dull it is, and encoded back into the bus from where it arrives. A moving
 *  source just changes how the bus reads at the twelve, with no Doppler and nothing rebuilt. What an
 *  image loses to scattering and the mixing time is not heard here; it comes out beside the field, a
 *  signal a virtual source, for the tail to take in.
 *
 *  The level is matched, not assumed: copies of one image close in time add their energies, copies
 *  together their amplitudes, so the reflections summed over six directions match what one exact
 *  source would give, and the bus's read is scaled to that.
 */
namespace bambi {

inline constexpr int kEarlyOrder = 3;  ///< the order reflections are read and placed at
inline constexpr int kEarlyChannels = 16;
inline constexpr int kCutoffClasses = 10;  ///< 800 Hz .. 20 kHz, about half an octave apart
inline constexpr double kMaxReflectionSeconds = 1.0;

/// A reflection that is both late and quiet is encoded at order 1 rather than kEarlyOrder (four gains
/// a sample instead of sixteen): late reflections are heard as a cluster, not as directions, so the
/// cost goes with their count and what is lost goes with an energy. Either constant past its range
/// narrows nothing.
inline constexpr double kWideUntilMixingTimes = 1.0;
inline constexpr double kWideAboveDb = -20.0;  ///< of the loudest reflection's amplitude

class EarlyReflections {
public:
    /// Allocates everything. `busOrder` is the field's; reflections use its first 16 channels, or all if fewer.
    void prepare(int busOrder, double sampleRate);

    /// A room and the distance its sources are assumed at. Real-time safe: bounded work, no allocation.
    void setRoom(const Room& room, double distance) noexcept;

    /// A gain on each virtual source's read of the bus, reached over `hopFrames` from wherever the
    /// last one had got to, so a moving region does not step the input of twelve delay lines.
    void setSourceGains(const std::array<float, kVirtualSources>& gains, int hopFrames) noexcept;

    void reset() noexcept;

    /// `in` is `frames` interleaved frames of the bus. `field` is the same shape and is overwritten
    /// with the reflections; `scattered` is `frames` x kVirtualSources and is overwritten with what
    /// each virtual source sends the tail. Neither may be `in`.
    void process(const float* in, float* field, float* scattered, int frames) noexcept;

    int liveTaps() const { return liveTaps_; }               ///< taps heard as reflections
    int narrowedTaps() const { return narrowedTaps_; }       ///< of them, the late and quiet ones encoded at order 1
    double narrowedShare() const { return narrowedShare_; }  ///< share of mirror energy those taps carry
    double level() const { return level_; }                  ///< the matched gain on the bus's reflections
    double mirrorEnergy() const { return mirrorEnergy_; }    ///< the exact model's mirror energy, unit impulse
    double scatteredEnergy() const { return scatteredEnergy_; }  ///< and its scattered energy
    double firstDelaySeconds() const { return firstDelay_; }     ///< delay of the first thing heard
    /// Energy of every image arriving in the second half of the mixing time: what the room's whole
    /// level is read from.
    double lateWindowEnergy() const { return lateEnergy_; }
    double windowEnergy() const { return windowEnergy_; }
    double firstWallSeconds() const { return firstWall_; }  ///< when the first wall reflection arrives, averaged

private:
    struct Tap {
        int delay{0};
        int klass{0};
        float mirror{0.0f}, scattered{0.0f};
        int wide{kEarlyChannels};  ///< channels this tap is encoded into: 16 while it is early, 4 once late
        std::array<float, kEarlyChannels> encode{};  ///< mirror x level x the harmonics of where it arrives from
    };
    int busOrder_{3}, busChannels_{16}, used_{16}, liveTaps_{0}, narrowedTaps_{0};
    double fs_{48000.0}, level_{1.0}, mirrorEnergy_{0.0}, scatteredEnergy_{0.0}, firstDelay_{0.0}, narrowedShare_{0.0};
    double lateEnergy_{0.0}, windowEnergy_{0.0}, firstWall_{0.0};

    std::array<std::array<float, kEarlyChannels>, kVirtualSources> decode_{};  ///< bus -> virtual source
    std::array<Ramp<float>, kVirtualSources> gain_{};                          ///< the send
    int hop_{1}, sinceGain_{0};
    bool snapGain_{true}, plainGain_{true};
    std::array<std::vector<float>, kVirtualSources> line_;
    int lineSize_{1}, write_{0};
    std::array<std::array<Tap, kMaxImages>, kVirtualSources> taps_{};
    std::array<int, kVirtualSources> tapCount_{};
    std::array<BiquadCoeffs, kCutoffClasses> lowpass_{};
    struct ClassState {
        std::array<double, kEarlyChannels> x1{}, x2{}, y1{}, y2{};
    };  ///< sixteen channels of one low-pass
    std::array<ClassState, kCutoffClasses> state_{};
    std::array<bool, kCutoffClasses> classUsed_{};
    std::array<int, kCutoffClasses> classWide_{};         ///< widest tap in each dullness class
    std::array<std::vector<float>, kCutoffClasses> acc_;  ///< a class's sixteen channels over a chunk, a run a channel
    std::unique_ptr<BusReflections> bus_;
    std::vector<std::pair<int, double>> scratch_;  ///< for the level matching: (sample, amplitude)
};

}  // namespace bambi
