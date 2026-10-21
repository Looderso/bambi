// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>
#include <map>
#include <optional>
#include <vector>

#include "bambi/live/field.hpp"
#include "bambi/live/lanes.hpp"
#include "bambi/live/protocol.hpp"
#include "bambi/live/timeline.hpp"
#include "bambi/live/transport.hpp"
#include "bambi/patch/parameters.hpp"
#include "bambi/ui/ConfirmBox.h"
#include "bambi/ui/HitArea.h"

/*  The live host's timeline: Live's arrangement as guidance, and the host's automation over it.
 *
 *    - the band: Live's transport, asked of Live -- play, stop -- the song position as it is heard, the tempo, and
 *      how long Live took to give its arrangement;
 *    - the ruler: the bars, Live's locators and its loop; a click locates, to a locator near it or else the beat;
 *    - a row per Live track, its name in the column on the left as the tree lays it out, its arrangement clips as
 *      named blocks in its colour, read-only; after Live's, a row for Main, its chain and the Decoders;
 *    - under a row, its lanes: one parameter of one of the row's instances each -- its Encoder, an insert, a
 *      Decoder -- drawn against the song. A click puts a point, snapped to the beat, the locators and the row's
 *      clip edges; a press on a point takes it, and a drag moves it, shift without snapping -- a drag from empty
 *      ground puts one and moves it; a double click removes one. A lane whose instance does not play now is kept,
 *      greyed;
 *    - the panel on the right, for the row chosen in the column: its lanes, each with a cross that removes it
 *      after asking, and "+ lane", a menu of the row's instances, then of a group of parameters, then of one;
 *    - the playhead, where Live is heard, over the ruler and the rows.
 *
 *  The session view, from the band's switch, as Live lays it out: a column per track, a row per scene, each clip in
 *  its colour with its name, the one Live plays marked -- no time in it. A click chooses a clip, which opens in a
 *  pane at the foot: its own ruler in its own bars, its clip lanes -- which follow that clip wherever it is
 *  launched, looping with it -- and a playhead where it is. The pane is as tall as its lanes need, four at most,
 *  and scrolls past them; the grid scrolls up and down, and sideways with the wheel or shift. The song's time is the
 *  arrangement view's alone; Main has no clips.
 *
 *  The arrangement view shows the whole song until zoomed: command and the wheel, or a pinch, zooms about the
 *  pointer, down to a bar; a sideways swipe, or shift and the wheel, moves along the song. It changes the lanes, never the engine: `lanesChanged` says what they are now.
 *  Clicks on release; escape steps back out of "+ lane". Message thread; it redraws itself while Live plays.
 */
namespace bambi::livehost {

class TimelinePage final : public ui::HitArea, private juce::Timer {
public:
    TimelinePage();

    /// What the page shows: Live's set and arrangement, the shape and the field it makes, the lanes, and which of
    /// the lanes play now.
    struct Shown {
        live::Tree tree;
        live::Arrangement arrangement;
        live::Shape shape;
        live::Field field;
        std::vector<live::Lane> lanes;
    };
    void show(Shown shown);
    /// A track's session clip as Live reported it, now: its playhead runs on from here.
    void slotPlayed(const live::SlotPlay& report);
    /// What of the song the arrangement view shows, as zooming and scrolling leave it. For checks.
    void showTime(live::TimeView view) {
        time_ = live::fitted(view, length_);
        repaint();
    }
    /// The session view, or the arrangement's, as the band's switch chooses. For checks.
    void showSession(bool on) {
        view_ = on ? View::Session : View::Arrangement;
        repaint();
    }

    std::function<void(const live::Command&)> command;          ///< the transport, asked of Live
    std::function<live::Playhead()> heard;                      ///< where Live is heard now
    std::function<void(std::vector<live::Lane>)> lanesChanged;  ///< the lanes, as the user left them
    std::function<bool(const live::Lane&)> plays;               ///< whether a lane's instance plays now
    std::function<void()> refresh;  ///< Live's arrangement asked for again: shown, a view switched, every while
    void visibilityChanged() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    void mouseMagnify(const juce::MouseEvent& e, float scale) override;

private:
    //  An instance a row holds, a lane's target.
    struct Owned {
        live::Instance instance{live::Instance::Encoder};
        std::uint64_t owner{0};
        live::Effect effect{live::Effect::Echo};
        juce::String name;
    };
    static constexpr live::TrackId kMainRow = live::kMain;

    void timerCallback() override;
    float xOf(double ppq) const;
    double ppqOf(float x) const;
    void band(juce::Graphics& g, const live::Playhead& at);
    void ruler(juce::Graphics& g, float top);
    void panel(juce::Graphics& g, juce::Rectangle<float> area);
    void menu(juce::Graphics& g);
    //  Where time runs across a lane: the song's, or one clip's, from its loop's start.
    struct Axis {
        float left{0.0f}, width{1.0f};
        double start{0.0}, length{1.0};  ///< quarter notes
        std::vector<double> edges;       ///< what a point snaps to beside the grid
        float x(double ppq) const { return left + static_cast<float>((ppq - start) / length) * width; }
        double at(float px) const { return start + static_cast<double>((px - left) / width) * length; }
    };
    float lane(juce::Graphics& g, live::Lane& l, float y, const Axis& axis);
    std::vector<Owned> ownedBy(live::TrackId row) const;
    std::optional<Owned> ownerOf(const live::Lane& l) const;
    juce::String laneName(const live::Lane& l) const;
    bool belongs(const live::Lane& l, live::TrackId row) const;
    void changed();
    void closeMenu();  ///< "+ lane" closed, from whatever level it had reached

    Shown s_;
    double length_{64.0};              ///< quarter notes of song there are to show
    live::TimeView time_{0.0, 1.0e9};  ///< what of it the arrangement view shows: the whole song until zoomed
    float left_{0.0f}, width_{1.0f};
    bool wasPlaying_{false};
    int sinceRefresh_{0};         ///< timer ticks since the arrangement was last asked for
    float pressedAt_{0.0f};       ///< where the ruler was pressed: a click there locates
    juce::Point<float> pressed_;  ///< where a lane was last pressed: a click puts, a double click removes, there
    bool tookPoint_{false};       ///< the last press on a lane took a point under it
    enum class View { Arrangement, Session };
    View view_{View::Arrangement};
    std::map<live::TrackId, int> chosenSlot_;  ///< the clip each track's row shows in the session view
    struct Seen {
        live::SlotPlay report;
        double atMs{0.0};  ///< when it came, on the high-resolution millisecond counter
    };
    std::map<live::TrackId, Seen> playing_;  ///< each track's session clip as last reported
    float paneTop_{0.0f};   ///< the session view's pane, as last drawn: a wheel below it scrolls its lanes
    int paneLanes_{0};      ///< the chosen clip's lanes
    int paneScroll_{0};     ///< the first of them in view
    float sideways_{0.0f};  ///< pixels the session grid is scrolled to the right
    const live::SessionClip* chosenClip(live::TrackId track) const;
    void paintSession(juce::Graphics& g, float top, const live::Playhead& at, float right);
    void clipPane(juce::Graphics& g, juce::Rectangle<float> area, const live::Playhead& at,
                  const live::SessionClip* clip);
    juce::Rectangle<float> addAt_;         ///< "+ lane", as last drawn: its menu drops from it
    std::optional<live::TrackId> chosen_;  ///< the row the panel shows
    //  "+ lane", as far as it has come: the instance, then the group
    bool adding_{false};
    std::optional<std::size_t> addInstance_;
    std::optional<ParamGroup> addGroup_;
    //  a point being dragged: its lane, and where in it
    std::uint64_t dragLane_{0};
    std::size_t dragPoint_{0};
    ui::ConfirmBox confirm_;
};

}  // namespace bambi::livehost
