// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <span>
#include <vector>

/*  A signal at one sampling rate as the same signal at another, by a windowed sinc: an impulse response
 *  measured at 44.1 kHz for a host running at 48. Band-limited to the lower of the two Nyquists, so a
 *  downsampling folds nothing back. Allocates; for files on load, not for audio.
 */
namespace bambi {

/// `out` gets round(in.size() * toHz / fromHz) samples. The sinc reaches `halfWidth` input samples each side under
/// a Hann window; 32 holds a sine to about -70 dB.
void resampleSinc(std::span<const float> in, double fromHz, double toHz, std::vector<float>& out, int halfWidth = 32);

}  // namespace bambi
