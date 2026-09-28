// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vector>

/*  What a plugin's engine publishes for the energy picture: the order it was prepared at, and the
 *  covariance of what leaves it a window -- with, for a field effect, the covariance of what
 *  arrived beside it, so the picture can say which energy is the plugin's. An encoder has nothing
 *  arriving and hands over an empty one. Message thread.
 */
namespace bambi::host {

class EnergyFeed {
public:
    virtual ~EnergyFeed() = default;

    /// What `prepareToPlay` built for; negative until it has run.
    virtual int engineOrder() const noexcept = 0;
    /// The newest pair, if one has landed since the last call. `arrived` comes back empty from a plugin that has none.
    virtual bool takeCovariances(std::vector<float>& added, std::vector<float>& arrived) = 0;
    /// Somebody is looking, or nobody is. The audio thread gathers only while somebody is: gathering for a picture nobody draws cost 1.47% of a core at order 7, in every instance.
    virtual void wantEnergy(bool wanted) noexcept = 0;
};

}  // namespace bambi::host
