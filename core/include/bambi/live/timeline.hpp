// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

#include "bambi/live/protocol.hpp"

/*  The live host's timeline, as numbers: where a quarter note is drawn, what a ruler click locates to, how a song
 *  position reads, how long the song is. Times in quarter notes from the song's start; four to a bar until Live
 *  sends its time signature. Pure; for the message thread.
 */
namespace bambi::live {

inline constexpr int kBeatsPerBar = 4;

/// A song position as Live shows it: "bar.beat.sixteenth", each counted from 1.
std::string barsBeats(double ppq);

/// How long the song is to draw: past its last clip, locator and loop end, in whole bars, at least `least` bars.
double songLength(const Arrangement& a, int least = 16);

/// Where a ruler click at `ppq` locates: the nearest locator within `tolerance` quarter notes, else the nearest
/// `grid` -- a bar, a beat -- never before the song's start.
double locateAt(double ppq, const Arrangement& a, double grid, double tolerance);

/// The position Live is heard at: `ppq` as the host plays it, less the samples between that and the speakers.
double heardAt(double ppq, double bpm, double rate, int samplesLate);

}  // namespace bambi::live
