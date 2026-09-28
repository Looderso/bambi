// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>

/*  The layout of Reverb's tail: the numbers a feedback delay network is built with, as opposed to the
 *  ones it is played with. Line lengths, each line's allpass, the diffuser's delays, shuffles and
 *  polarity flips, and the output's -- all from one seed, so every build of the same room is the same
 *  network and a render repeats. Follows two things only: how long the lines are (the room's volume,
 *  as `lineScale`) and how long the diffuser is (the mixing time). Fixed-size, so making one
 *  allocates nothing.
 */
namespace bambi {

inline constexpr int kMaxTailLines = 32;
inline constexpr int kDiffuserSteps = 4;

struct TailPlan {
    int lines{16};
    std::array<int, kMaxTailLines> length{};   ///< samples; primes, no two alike, long and short interleaved
    std::array<int, kMaxTailLines> allpass{};  ///< samples; primes over 5..15 ms x scale, shuffled
    std::array<std::array<int, kMaxTailLines>, kDiffuserSteps> diffuserDelay{};
    std::array<std::array<int, kMaxTailLines>, kDiffuserSteps>
        diffuserFrom{};  ///< channel k takes channel diffuserFrom[k]...
    std::array<std::array<float, kMaxTailLines>, kDiffuserSteps> diffuserSign{};  ///< ...times this
    std::array<int, kMaxTailLines> outFrom{};
    std::array<float, kMaxTailLines> outSign{};

    /// All the lines' lengths together, in seconds: Schroeder and Logan ask for at least 0.15 x RT60
    /// of it for a tail without colour.
    double totalSeconds(double sampleRate) const;
};

/// `lines` is a power of two up to kMaxTailLines, since the diffuser mixes with a Hadamard matrix.
/// `lineScale` is Room::lineScale; lines run 100 to 200 ms times it. `mixingTime` is
/// Room::mixingTime: the diffuser's four steps, in proportions 20:40:80:160, add up to it.
TailPlan planTail(int lines, double sampleRate, double lineScale, double mixingTime);

/// The longest a line, an allpass and a diffuser delay can be at a sample rate: what to allocate.
struct TailLimits {
    int line, allpass, diffuser;
};
TailLimits tailLimits(double sampleRate);

}  // namespace bambi
