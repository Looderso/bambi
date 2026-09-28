// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string_view>

/*  A tap's time, kept both in sixteenths and in milliseconds; `synced` says which is live. Swing
 *  delays the offset, not the period. tapTimes() is what the engine plays and the UI shows; it
 *  never changes what is stored.
 */
namespace bambi {

inline constexpr double kMinTapSeconds = 0.02;
inline constexpr int kMaxSteps = 16;
inline constexpr double kMaxSwing = 0.9;

/// The longest tap at an order. The rings are allocated at this length.
constexpr double maxTapSeconds(int order) { return order <= 3 ? 2.4 : order <= 5 ? 1.6 : 1.2; }

/// What a stored millisecond value is held to: the largest cap at any order.
inline constexpr double kMaxStoredSeconds = 2.4;

struct TapTiming {
    bool synced{true};
    int steps{4};  ///< sixteenths a pass lasts
    int offsetSteps{0};
    double swing{0.0};  ///< a fraction of a step added to the offset
    double ms{375.0};
    double offsetMs{0.0};
};

struct TapTimes {
    double periodSeconds{0.375};
    double offsetSeconds{0.0};
    int stepsPlayed{0};  ///< synced: the steps the period came to; 0 when free or none fits
};

double stepSeconds(double bpm);

/// What the tap plays. A synced period over the order's cap is halved until it fits, so it stays
/// on the grid; one under kMinTapSeconds is doubled. Free times and offsets are clamped.
TapTimes tapTimes(const TapTiming& t, double bpm, int order);

/// The milliseconds take the tap's current time, so leaving sync moves nothing.
TapTiming leaveSync(TapTiming t, double bpm);

/// The nearest whole step; the offset's whole steps, with the remainder as swing.
TapTiming returnToSync(TapTiming t, double bpm);

/// Where the offset falls inside the loop, 0..1.
double placeInLoop(const TapTimes& times);

/// "1/16", "1/8", "1/8.", "1/4", "1/4.", "1/2", "1/2.", "1 bar", and "n/16" for the rest.
std::string_view stepName(int steps);

}  // namespace bambi
