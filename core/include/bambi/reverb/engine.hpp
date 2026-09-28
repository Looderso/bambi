// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <vector>

#include "bambi/dsp/ramp.hpp"
#include "bambi/region/projection.hpp"
#include "bambi/reverb/early.hpp"
#include "bambi/reverb/room.hpp"
#include "bambi/reverb/tail.hpp"

/*  Reverb's engine: one room, heard as its reflections and its tail.
 *
 *      bus --+--> early reflections ------------------------------+--> x early gain --+
 *            |        | what scattered, a signal a virtual source |                   +--> out
 *            |        v                                           |                   |
 *            +--> read at the lines' directions --> tail --> placed at them --> x tail gain
 *
 *  Reflections and tail are one decay: the reflections arriving in the second half of the mixing time
 *  give the level K of the exponential decay they are the start of,
 *
 *      K = E(t_mix/2 .. t_mix) / (t_mix/2) * RT60/13.8 * e^(13.8 * 0.75 * t_mix / RT60)
 *
 *  and whatever of K is not a discrete reflection comes out of the tail; the two are held at unit
 *  energy together, so distance moves energy between them and the level moves both. A dry gain is
 *  mixed in at the very end, as an output stage the balance above never sees.
 *
 *  A room that changes size does not restart its tail, since the old network is still the sound of a
 *  room decaying: a new empty network is built and fed the same input, and the two fade past each
 *  other over kTailFadeSeconds in equal power. A line-count change still restarts, since the networks
 *  then have different numbers of outputs.
 *
 *  Send and return cost almost nothing: the send is the region's value at each virtual source and
 *  line, gains rather than a matrix; the return uses the same gains for the lines' placement but puts
 *  reflections back over the whole bus via the shared projection (region/projection.hpp).
 */
namespace bambi {

inline constexpr int kReverbHop = 256;

/// Cross-fade time for a size change: long enough not to read as a level move, short enough to follow a dragged knob.
inline constexpr double kTailFadeSeconds = 0.25;

/// What the engine is played with, in its own units: seconds, radians, linear gains.
struct ReverbFrame {
    RoomSettings room;
    double distance{6.0};  ///< sets only the dry's level; the reflections' geometry ignores it
    double levelDb{0.0};   ///< the wet level: what the room comes out at
    double dryGain{0.0};   ///< linear, 0 for silence; a trim on the direct-to-reverberant ratio, not an output level
    double preDelayTrimSeconds{0.0};  ///< added to the pre-delay the room gives
    double lowCutHz{150.0}, highCutHz{11000.0};
    int tailLines{16};  ///< 8, 16 or 32: the quality switch's largest lever
    int tailOrder{3};   ///< the order the tail is placed at; never above the bus's
    bool drift{true};

    Region send{};  ///< what of the field enters the room; amount folds into the gains it scales
    double sendAmount{1.0};
    Region returnRegion{};  ///< where what the room made is put back
    double returnAmount{1.0};
};

/// What the room works out to: for the engine, and for a readout.
struct ReverbBalance {
    double roomEnergy{1.0};                ///< K
    double mirrorEnergy{0.0};              ///< of it, heard as discrete reflections
    double tailEnergy{0.0};                ///< of it, left to the tail
    double earlyGain{1.0}, tailGain{1.0};  ///< linear, before the level
    double dryGain{0.0};  ///< the dry as actually played, for a readout: the trim times the ratio at this distance
    double preDelaySeconds{0.0};
};

class ReverbEngine {
public:
    void prepare(int busOrder, double sampleRate);
    void reset() noexcept;

    /// A control step. Real-time safe; restarts the tail only if the line count changed.
    void set(const ReverbFrame& settings) noexcept;

    /// `frames` interleaved frames of the bus in, the same out. `in` and `out` may be the same buffer.
    void process(const float* in, float* out, int frames) noexcept;

    const ReverbBalance& balance() const { return balance_; }
    const Room& room() const { return room_; }
    int channels() const { return channels_; }
    int lines() const { return lines_; }
    /// A line's read distance in the live network, in samples: for tests, since level or decay alone can't tell which layout is in force.
    double tailDelayOf(int line) const noexcept { return tail_[static_cast<std::size_t>(live_)].delayOf(line); }

private:
    static constexpr int kChunk = 64;
    int busOrder_{3}, channels_{16}, lines_{16}, tailChannels_{16};
    double fs_{48000.0};
    bool configured_{false};
    ReverbFrame settings_;
    Room room_;
    ReverbBalance balance_;
    EarlyReflections early_;
    std::array<FdnTail, 2>
        tail_;  ///< two networks; `tail_[live_]` is current, the other fades out while `fading_ >= 0`
    int live_{0}, fading_{-1}, fadeAt_{0}, fadeFrames_{1};
    bool played_{false};  ///< nothing has been heard yet, so a new layout costs nothing to adopt outright
    double planScale_{-1.0}, planMixing_{-1.0};        ///< scale/mixing time the live layout was built from
    double leavingTailGain_{1.0}, leavingRatio_{1.0};  ///< the leaving network's own normalisation ratio
    TailPlan pending_{};
    bool havePending_{false};
    RegionOperator returnOp_;
    Region sendNow_{}, returnNow_{};
    bool regionsSet_{false}, returnPlain_{true};
    std::array<Vec3, kMaxTailLines> lineDirection_{};
    std::array<std::array<float, 4>, kMaxTailLines> read_{};  ///< bus (order 1) -> a line
    std::array<std::array<float, kVirtualSources>, kMaxTailLines> fromScattered_{};
    std::array<std::array<float, kEarlyChannels>, kMaxTailLines> place_{};  ///< a line -> the bus, at the tail order
    // The regions, as gains, recomputed only when a region or the lines move.
    std::array<float, kVirtualSources> sendSourceRaw_{};
    std::array<float, kMaxTailLines> sendLineRaw_{}, returnLineRaw_{};
    std::array<Ramp<float>, kMaxTailLines> sendLine_{}, returnLine_{};
    std::vector<float> clean_, field_, placed_, scattered_, lineIn_, lineScattered_, lineOut_, lineOutOld_;
    Ramp<double> earlyGain_{}, tailGain_{}, dryGain_{};
    double builtDistance_{6.0};
    int sinceSet_{kReverbHop};
    bool snap_{true};

    void layOutLines(int lines) noexcept;
    void setRegions(const ReverbFrame& settings) noexcept;
    /// Takes up a layout: fades to it if the line count matches, else adopts it outright (emptying the network).
    void adoptPlan(const TailPlan& plan) noexcept;
};

}  // namespace bambi
