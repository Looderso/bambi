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

/// What of the song the arrangement view shows: from `start`, `span` quarter notes wide.
struct TimeView {
    double start{0.0}, span{64.0};
    friend bool operator==(const TimeView&, const TimeView&) = default;
};
inline constexpr double kNarrowestView = 4.0;  ///< a bar, as far as zooming in goes

/// `view` held inside a song `length` quarter notes long: no wider than the song, no narrower than a bar, and never
/// past either end.
TimeView fitted(TimeView view, double length);
/// `view` zoomed by `factor` -- above 1 wider, below 1 narrower -- about `anchor`, which stays where it is drawn.
TimeView zoomed(const TimeView& view, double factor, double anchor, double length);
/// `view` moved along the song by `beats`.
TimeView scrolled(const TimeView& view, double beats, double length);

/// A song position as Live shows it: "bar.beat.sixteenth", each counted from 1.
std::string barsBeats(double ppq);

/// How long the song is to draw: past its last clip, locator and loop end, in whole bars, at least `least` bars.
double songLength(const Arrangement& a, int least = 16);

/// `ppq` snapped: to the nearest of `edges` within `tolerance` quarter notes, else to the nearest `grid` -- a bar, a
/// beat; none snaps it -- never before the song's start.
double snapAt(double ppq, const std::vector<double>& edges, double grid, double tolerance);

/// What a time on a row snaps to beside the grid: the locators, and the edges of `track`'s clips; 0 for none.
std::vector<double> edgesOf(const Arrangement& a, TrackId track);

/// Where a ruler click at `ppq` locates: to a locator within `tolerance`, else to `grid`.
double locateAt(double ppq, const Arrangement& a, double grid, double tolerance);

/// The grid a timeline drawn `pixelsPerBeat` wide snaps to: a bar while beats are narrow, a beat, a sixteenth.
double gridFor(double pixelsPerBeat);

/// The position Live is heard at: `ppq` as the host plays it, less the samples between that and the speakers.
double heardAt(double ppq, double bpm, double rate, int samplesLate);

}  // namespace bambi::live
