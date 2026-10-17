// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

#include "bambi/live/protocol.hpp"
#include "bambi/live/transport.hpp"
#include "bambi/ui/HitArea.h"

/*  The live host's timeline: Live's arrangement, as guidance for placing automation, never edited here.
 *
 *    - the band: Live's transport, asked of Live -- play, stop -- the song position as it is heard, the tempo, and
 *      how long Live took to give its arrangement;
 *    - the ruler: the bars, Live's locators and its loop; a click locates, to a locator near it or else the beat;
 *    - a row per Live track, its name in the column on the left as the tree lays it out, its arrangement clips as
 *      named blocks in its colour; a group and a return have none of their own;
 *    - the playhead, where Live is heard, over the ruler and the rows.
 *
 *  The whole song fits the width. Clicks on release. Message thread; it redraws itself while Live plays.
 */
namespace bambi::livehost {

class TimelinePage final : public ui::HitArea, private juce::Timer {
public:
    TimelinePage();

    /// Live's set and arrangement as the device last said.
    void show(const live::Tree& tree, const live::Arrangement& arrangement);

    std::function<void(const live::Command&)> command;  ///< the transport, asked of Live
    std::function<live::Playhead()> heard;              ///< where Live is heard now

    void paint(juce::Graphics& g) override;

private:
    void timerCallback() override;
    float xOf(double ppq) const;
    double ppqOf(float x) const;
    void band(juce::Graphics& g, const live::Playhead& at);
    void ruler(juce::Graphics& g, float top);

    live::Tree tree_;
    live::Arrangement arrangement_;
    double length_{64.0};  ///< quarter notes the width shows
    float left_{0.0f}, width_{1.0f};
    bool wasPlaying_{false};
    float pressedAt_{0.0f};  ///< where the ruler was pressed: a click there locates
};

}  // namespace bambi::livehost
