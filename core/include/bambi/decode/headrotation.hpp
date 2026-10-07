// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vector>

#include "bambi/link/latest.hpp"
#include "bambi/math/shrotation.hpp"

/*  The field turned against the head: a world-fixed scene heard from a head that moves. The head's
 *  orientation in the world arrives from any thread -- the tracker's receiver, or yaw, pitch and roll
 *  as automated -- and the field is turned by its inverse, so a source stays where it is in the room
 *  while the head turns. Every hop of kRotationHop frames the stage glides toward the newest
 *  orientation (an exponential approach, kRotationGlideSeconds to settle most of the way), rebuilds
 *  its blocks when it moved, and crossfades within the hop between the previous turn and the new one,
 *  so a tracker's steps and a recentre never snap. The same bits however the frames are cut.
 */
namespace bambi {

inline constexpr int kRotationHop = 64;
inline constexpr double kRotationGlideSeconds = 0.03;  ///< the time constant of the approach

class HeadRotation {
public:
    /// Allocates: the blocks for `order` and scratch for one hop.
    bool prepare(int order, double sampleRate);

    /// Any thread: the head's rotation in the world, row-major. The newest one wins.
    void setOrientation(const Mat3& headInWorld) noexcept { target_.write(headInWorld); }

    /// Audio thread. `frames` interleaved frames of numChannels(order) in, the same out; may not alias.
    void process(const float* in, float* out, int frames) noexcept;
    /// Goes straight to the newest orientation at the next hop, without the glide; forgets the hop position.
    void reset() noexcept {
        snap_ = true;
        pos_ = 0;
        moving_ = false;
    }

    /// The head's orientation the turn in force follows, for drawing. Audio-thread owned; a reader elsewhere sees a
    /// value that is at most one hop old, and never torn beyond one block's worth.
    const Mat3& current() const noexcept { return current_; }

private:
    void turnTo(const Mat3& head) noexcept;  // the blocks for a head at `head`

    int order_{0}, channels_{0};
    double alpha_{1.0};  // the share of the way a hop takes toward the goal
    LatestValue<Mat3> target_;
    Mat3 goal_{kIdentity3}, current_{kIdentity3};
    bool snap_{true}, settled_{true}, moving_{false};
    int pos_{0};  // frames into the current hop
    std::vector<double> blocks_;
    std::vector<float> forward_, previous_, inverseScratch_;  // column-major float blocks
    std::vector<float> a_, b_;                                // one hop each, turned by the previous and the new
};

}  // namespace bambi
