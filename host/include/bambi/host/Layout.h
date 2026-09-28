// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

/// What a plugin tells a host about its buses: every plugin speaks (N+1)^2 channels, but the encoder takes a mono or stereo source while Echo and Reverb take the field itself back.
namespace bambi::host {

/// The highest order this suite offers a host. 121 channels.
inline constexpr int kMaxHostOrder = 10;

/// The order whose (N+1)^2 channels are exactly `channels`; -1 when it is not a perfect square.
int orderForChannels(int channels) noexcept;

/*  The highest order whose (N+1)^2 channels fit in `channels`, capped at kMaxHostOrder; -1 for none.
 *  Accepting only exact counts fails on a real track: REAPER offers only even channel counts, so
 *  orders 2, 4, 6, 8 and 10 could never be asked for. Channels past the order are left silent. */
int orderThatFits(int channels) noexcept;

/// A layout as a line of text, for the readout and the layout log.
juce::String describeLayout(const juce::AudioProcessor::BusesLayout& layout);

/// A field effect's buses: in and out are both ambisonic, at the same order. A sidechain may be any width, including none.
bool fieldEffectLayoutSupported(const juce::AudioProcessor::BusesLayout& layout);

}  // namespace bambi::host
