// SPDX-License-Identifier: GPL-3.0-or-later
#include "Timeline.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "Plugins.h"
#include "bambi/live/timeline.hpp"
#include "bambi/ui/Draw.h"
#include "bambi/ui/ProductName.h"
#include "bambi/ui/Widgets.h"

namespace bambi::livehost {

namespace {

namespace colour = ui::theme::colour;
namespace tl = ui::theme::timeline;
const juce::String kDash = juce::String::fromUTF8("\xe2\x80\x93");
using live::Node;

//  How deep a node sits: 0 at the top, one more for each group it plays into.
int depthOf(const live::Tree& tree, const Node& n) {
    int depth = 0;
    const Node* at = &n;
    while (at->parent && depth < 64) {
        const auto up =
            std::find_if(tree.nodes.begin(), tree.nodes.end(), [&](const Node& x) { return x.id == *at->parent; });
        if (up == tree.nodes.end()) break;
        at = &*up;
        ++depth;
    }
    return depth;
}

const char* groupName(ParamGroup g) {
    switch (g) {
        case ParamGroup::Transform: return "transform";
        case ParamGroup::Motion: return "motion";
        case ParamGroup::Render: return "render";
        case ParamGroup::Input: return "input";
        case ParamGroup::Modulation: return "modulation";
        case ParamGroup::Lfo: return "lfo";
        case ParamGroup::Envelope: return "envelope";
        case ParamGroup::Region: return "region";
    }
    return "";
}

juce::String effectName(live::Effect e) { return e == live::Effect::Reverb ? "reverb" : "echo"; }

//  A clip as Live colours it: filled faintly, its colour along its top, its name when there is room -- in the
//  arrangement's rows, the session's chosen clip, and, without bar or name, a session slot.
void clipBlock(juce::Graphics& g, juce::Rectangle<float> block, const std::string& colourHex, const juce::String& name,
               bool full) {
    const juce::Colour ink = colour::ofLive(juce::String(colourHex), colour::label);
    g.setColour(ink.withAlpha(tl::clipFill));
    g.fillRect(block);
    if (!full) return;
    g.setColour(ink);
    g.fillRect(block.withHeight(tl::clipBar));
    if (block.getWidth() > tl::barLabel)
        ui::text(g, name, ui::smallFont(), colour::text, block.reduced(tl::clipInset, 0.0f).withTrimmedTop(tl::clipBar),
                 juce::Justification::topLeft);
}

}  // namespace

TimelinePage::TimelinePage() {
    setWantsKeyboardFocus(true);
    addChildComponent(confirm_);
    confirm_.answered = [this] { repaint(); };
    startTimer(tl::frameMs);
}

void TimelinePage::resized() { confirm_.setBounds(getLocalBounds()); }

void TimelinePage::show(Shown shown) {
    //  a drag holds its own copy of the lanes until it is let go
    if (dragLane_ != 0) shown.lanes = s_.lanes;
    s_ = std::move(shown);
    length_ = live::songLength(s_.arrangement);
    time_ = live::fitted(time_, length_);
    repaint();
}

void TimelinePage::slotPlayed(const live::SlotPlay& report) {
    playing_[report.id] = {report, juce::Time::getMillisecondCounterHiRes()};
    if (view_ == View::Session) repaint();
}

const live::SessionClip* TimelinePage::chosenClip(live::TrackId track) const {
    const auto slots = std::find_if(s_.arrangement.session.begin(), s_.arrangement.session.end(),
                                    [&](const live::TrackSlots& t) { return t.id == track; });
    if (slots == s_.arrangement.session.end() || slots->clips.empty()) return nullptr;
    const auto chosen = chosenSlot_.find(track);
    const int slot = chosen != chosenSlot_.end() ? chosen->second : slots->clips.front().slot;
    for (const live::SessionClip& c : slots->clips)
        if (c.slot == slot) return &c;
    return &slots->clips.front();
}

void TimelinePage::closeMenu() {
    adding_ = false;
    addInstance_.reset();
    addGroup_.reset();
}

void TimelinePage::changed() {
    if (lanesChanged) lanesChanged(s_.lanes);
    repaint();
}

bool TimelinePage::keyPressed(const juce::KeyPress& key) {
    if (key != juce::KeyPress::escapeKey || !adding_) return false;
    if (addGroup_)
        addGroup_.reset();
    else if (addInstance_)
        addInstance_.reset();
    else
        adding_ = false;
    repaint();
    return true;
}

void TimelinePage::visibilityChanged() {
    if (isVisible() && refresh) refresh();
    sinceRefresh_ = 0;
}

void TimelinePage::timerCallback() {
    //  while Live plays, and once more as it stops, so the playhead rests where it stopped
    //  a session clip plays only while Live does
    const bool playing = isShowing() && heard && heard().playing;
    if (playing || wasPlaying_) repaint();
    wasPlaying_ = playing;
    //  Live does not say when a clip is made or moved: asked again every so often while the page is open; the
    //  device reads only as often as the read's own cost allows
    if (isShowing() && ++sinceRefresh_ * tl::frameMs >= tl::refreshMs) {
        sinceRefresh_ = 0;
        if (refresh) refresh();
    }
}

float TimelinePage::xOf(double ppq) const {
    return left_ + static_cast<float>((ppq - time_.start) / time_.span) * width_;
}
double TimelinePage::ppqOf(float x) const {
    return time_.start + static_cast<double>((x - left_) / width_) * time_.span;
}

//  The bars the view shows, the first to the last, partly shown included.
static std::pair<int, int> barsIn(const live::TimeView& t) {
    return {static_cast<int>(std::floor(t.start / live::kBeatsPerBar)),
            static_cast<int>(std::ceil((t.start + t.span) / live::kBeatsPerBar))};
}

std::vector<TimelinePage::Owned> TimelinePage::ownedBy(live::TrackId row) const {
    std::vector<Owned> out;
    if (row != kMainRow) {
        const auto sources = s_.field.sources();
        if (std::find(sources.begin(), sources.end(), row) != sources.end())
            out.push_back({live::Instance::Encoder, row, live::Effect::Echo, ui::productName(Product::Encoder)});
    }
    for (const live::Insert& i : s_.shape.insertsOn(row))
        out.push_back({live::Instance::Insert, i.id, i.effect, effectName(i.effect)});
    if (row == kMainRow)
        for (std::size_t k = 0; k < s_.field.decoders.size(); ++k)
            out.push_back({live::Instance::Decoder, s_.field.decoders[k], live::Effect::Echo,
                           s_.field.decoders.size() > 1 ? "decoder " + juce::String(static_cast<int>(k) + 1)
                                                        : juce::String("decoder")});
    return out;
}

bool TimelinePage::belongs(const live::Lane& l, live::TrackId row) const {
    if (l.instance == live::Instance::Encoder) return row != kMainRow && l.owner == row;
    if (l.instance == live::Instance::Decoder) return row == kMainRow;
    for (const live::Insert& i : s_.shape.insertsOn(row))
        if (i.id == l.owner) return true;
    return false;
}

std::optional<TimelinePage::Owned> TimelinePage::ownerOf(const live::Lane& l) const {
    if (l.instance == live::Instance::Encoder)
        return Owned{l.instance, l.owner, live::Effect::Echo, ui::productName(Product::Encoder)};
    if (l.instance == live::Instance::Decoder) return Owned{l.instance, l.owner, live::Effect::Echo, "decoder"};
    for (const live::Chain& c : s_.shape.chains)
        for (const live::Insert& i : c.inserts)
            if (i.id == l.owner) return Owned{l.instance, l.owner, i.effect, effectName(i.effect)};
    return std::nullopt;
}

juce::String TimelinePage::laneName(const live::Lane& l) const {
    const auto owner = ownerOf(l);
    if (!owner) return juce::String(l.param);
    const ParamManifest& m = manifestOf(owner->instance, owner->effect);
    const int at = m.byKey(l.param);
    return (at >= 0 ? juce::String(std::string(m[at].name)) : juce::String(l.param)) + ui::kSeparator + owner->name;
}

void TimelinePage::paint(juce::Graphics& g) {
    g.fillAll(colour::ground);
    clearRegions();
    const auto w = static_cast<float>(getWidth());
    const auto top = static_cast<float>(ui::theme::metrics::headerHeight);
    left_ = tl::tree;
    width_ = std::max(1.0f, w - tl::tree - tl::panel - tl::pad);
    const live::Playhead at = heard ? heard() : live::Playhead{};

    const float right = left_ + width_ + tl::pad;
    if (view_ == View::Session) {
        paintSession(g, top, at, right);
        panel(g, {right, top + tl::ruler, w - right, static_cast<float>(getHeight()) - top - tl::ruler});
        band(g, at);
        menu(g);
        return;
    }
    ruler(g, top);
    ui::ruleH(g, 0.0f, w, top + tl::ruler - 1.0f, colour::rule);
    const float rows = top + tl::ruler;
    setViewport(rows, static_cast<float>(getHeight()));
    {
        const ContentClip clip(*this, g);
        float y = rows - scroll();
        //  the time area: what of the song the view shows, never over the tree's column or the panel
        const juce::Rectangle<float> times{left_, rows, width_, static_cast<float>(getHeight()) - rows};
        {
            juce::Graphics::ScopedSaveState keep(g);
            g.reduceClipRegion(times.toNearestInt());
            const auto [firstBar, lastBar] = barsIn(time_);
            for (int bar = firstBar; bar <= lastBar; ++bar) {
                const float x = xOf(bar * live::kBeatsPerBar);
                g.setColour(bar % 4 == 0 ? colour::rule : colour::rowDivider);
                g.fillRect(x, rows, 1.0f, static_cast<float>(getHeight()) - rows);
            }
        }
        //  a row's header: its name, indented by its depth, a click choosing it for the panel
        const auto header = [&](live::TrackId id, const juce::String& name, int depth, const juce::String& kind) {
            const juce::Rectangle<float> row{0.0f, y, right, tl::row};
            if (chosen_ && *chosen_ == id) {
                g.setColour(colour::highlight);
                g.fillRect(row.withWidth(tl::tree));
            }
            g.setColour(colour::rowDivider);
            g.fillRect(row.getX(), row.getBottom() - 1.0f, row.getWidth(), 1.0f);
            const float indent = tl::pad + tl::indent * static_cast<float>(depth);
            const juce::Rectangle<float> head{indent, y, tl::tree - indent - tl::pad, tl::row};
            ui::text(g, name, ui::labelFont(), colour::text, head, juce::Justification::centredLeft);
            if (kind.isNotEmpty())
                ui::text(g, kind, ui::smallFont(), colour::label, head, juce::Justification::centredRight);
            addClickArea(row.withWidth(tl::tree), [this, id] {
                chosen_ = id;
                closeMenu();
                grabKeyboardFocus();
                repaint();
            });
        };
        const auto lanesOf = [&](live::TrackId id) {
            for (live::Lane& l : s_.lanes)
                if (belongs(l, id) && !l.clip())
                    y = lane(g, l, y, {left_, width_, time_.start, time_.span, live::edgesOf(s_.arrangement, id)});
        };
        bool returns = false;
        for (const Node& n : s_.tree.nodes) {
            if (n.kind == Node::Kind::Return && !returns) {
                returns = true;
                y = ui::drawGroupHeading(g, tl::pad, y + tl::pad, right - tl::pad, "returns");
            }
            header(n.id, juce::String(n.name), depthOf(s_.tree, n),
                   n.kind == Node::Kind::Group    ? "group"
                   : n.kind == Node::Kind::Return ? "return"
                                                  : "");
            //  its clips, as Live has them: read-only
            const auto clips = std::find_if(s_.arrangement.tracks.begin(), s_.arrangement.tracks.end(),
                                            [&](const live::TrackClips& t) { return t.id == n.id; });
            //  a track the session has: its arrangement waits for Back to Arrangement, as in Live
            const auto seen = playing_.find(n.id);
            const bool taken =
                seen != playing_.end() && (seen->second.report.overrides || seen->second.report.slot >= 0);
            juce::Graphics::ScopedSaveState keepClips(g);
            g.reduceClipRegion(times.toNearestInt());
            if (clips != s_.arrangement.tracks.end())
                for (const live::Clip& c : clips->clips) {
                    const float x0 = xOf(c.start), x1 = xOf(c.end);
                    const juce::Rectangle<float> block{x0, y + tl::clipInset, std::max(1.0f, x1 - x0 - 1.0f),
                                                       tl::row - 2.0f * tl::clipInset};
                    //  faint, without its bar or name, while the session has the track
                    clipBlock(g, block, c.colour, juce::String(c.name), !taken);
                }
            if (taken)
                ui::text(g, "the session's" + ui::kSeparator + "back to arrangement in Live", ui::smallFont(),
                         colour::label, {left_ + tl::gap, y, width_, tl::row}, juce::Justification::bottomLeft);
            y += tl::row;
            lanesOf(n.id);
        }
        y = ui::drawGroupHeading(g, tl::pad, y + tl::pad, right - tl::pad, "main");
        header(kMainRow, "main", 0, "chain" + ui::kSeparator + "decoders");
        y += tl::row;
        lanesOf(kMainRow);
        noteContentBottom(y + scroll());
        if (xOf(at.ppq) >= left_ && xOf(at.ppq) <= left_ + width_) {
            g.setColour(colour::playhead);
            g.fillRect(xOf(at.ppq), rows, 1.0f, static_cast<float>(getHeight()) - rows);
        }
    }
    if (xOf(at.ppq) >= left_ && xOf(at.ppq) <= left_ + width_) {
        g.setColour(colour::playhead);
        g.fillRect(xOf(at.ppq), top, 1.0f, tl::ruler);
    }
    panel(g, {right, rows, w - right, static_cast<float>(getHeight()) - rows});
    band(g, at);
    menu(g);
}

//  The session view's rows: every track that can hold clips, its slots in a strip, the chosen clip across the rest
//  with its clip lanes under it, and where Live is in it while it plays.
//  The session view, as Live lays it out: a column per track that can hold clips, its name at the top, a row per
//  scene, numbered on the left, each clip a cell in its colour with its name. The clip the panel shows is framed in
//  ink, the one Live plays marked as now; a click chooses one. No time here: the chosen clip's time is the pane's,
//  at the foot, as tall as its lanes need and no taller than kPaneLanes of them. The grid scrolls up and down, and
//  sideways when the tracks are more than its width.
void TimelinePage::paintSession(juce::Graphics& g, float top, const live::Playhead& at, float right) {
    const auto h = static_cast<float>(getHeight());
    //  the grid's own left edge: a column just wide enough for the scenes' numbers, not the tracks' names
    const float gridLeft = tl::sceneNumbers, gridWidth = std::max(1.0f, right - tl::pad - gridLeft);
    std::vector<const Node*> tracks;
    for (const Node& n : s_.tree.nodes)
        if (n.kind == Node::Kind::Track) tracks.push_back(&n);
    const int scenes = std::max(1, s_.arrangement.scenes);

    //  the pane first: its height is what the chosen clip's lanes need
    const live::SessionClip* clip = chosen_ ? chosenClip(*chosen_) : nullptr;
    paneLanes_ = 0;
    if (clip != nullptr)
        for (const live::Lane& l : s_.lanes)
            if (l.clip() && l.clipTrack == *chosen_ && l.slot == clip->slot) ++paneLanes_;
    const int shownLanes = std::clamp(paneLanes_, 1, tl::paneLanes);
    paneScroll_ = std::clamp(paneScroll_, 0, std::max(0, paneLanes_ - tl::paneLanes));
    paneTop_ = h - tl::ruler - tl::lane * static_cast<float>(shownLanes);

    //  the tracks' names over their columns, sideways with the grid
    const float columns = tl::sessionColumn * static_cast<float>(tracks.size());
    sideways_ = std::clamp(sideways_, 0.0f, std::max(0.0f, columns - gridWidth));
    const juce::Rectangle<float> heads{gridLeft, top, gridWidth, tl::ruler};
    const auto columnX = [&](std::size_t i) {
        return gridLeft + tl::sessionColumn * static_cast<float>(i) - sideways_;
    };
    {
        juce::Graphics::ScopedSaveState keep(g);
        g.reduceClipRegion(heads.toNearestInt());
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            const juce::Rectangle<float> head{columnX(i), top, tl::sessionColumn, tl::ruler};
            if (chosen_ && *chosen_ == tracks[i]->id) {
                g.setColour(colour::highlight);
                g.fillRect(head);
            }
            ui::text(g, juce::String(tracks[i]->name), ui::labelFont(), colour::text, head.reduced(tl::gap, 0.0f),
                     juce::Justification::centredLeft);
            const auto clickable = head.getIntersection(heads);
            if (!clickable.isEmpty())
                addClickArea(clickable, [this, id = tracks[i]->id] {
                    chosen_ = id;
                    closeMenu();
                    repaint();
                });
        }
    }
    ui::ruleH(g, 0.0f, right, top + tl::ruler - 1.0f, colour::rule);

    //  the scenes, each a row of cells
    const float rows = top + tl::ruler;
    setViewport(rows, paneTop_);
    {
        const ContentClip clipped(*this, g);
        const juce::Rectangle<float> grid{gridLeft, rows, gridWidth, paneTop_ - rows};
        float y = rows - scroll();
        for (int sc = 0; sc < scenes; ++sc) {
            ui::text(g, juce::String(sc + 1), ui::smallFont(), colour::label,
                     {0.0f, y, gridLeft - tl::gap, tl::sceneRow}, juce::Justification::centredRight);
            g.setColour(colour::rowDivider);
            g.fillRect(0.0f, y + tl::sceneRow - 1.0f, right, 1.0f);
            juce::Graphics::ScopedSaveState keep(g);
            g.reduceClipRegion(grid.toNearestInt());
            for (std::size_t i = 0; i < tracks.size(); ++i) {
                const juce::Rectangle<float> box{columnX(i) + tl::slotGap, y + tl::slotGap,
                                                 tl::sessionColumn - 2.0f * tl::slotGap,
                                                 tl::sceneRow - 2.0f * tl::slotGap};
                const auto clickable = box.getIntersection(grid);
                if (clickable.isEmpty()) continue;
                const live::SessionClip* c = nullptr;
                for (const live::TrackSlots& t : s_.arrangement.session)
                    if (t.id == tracks[i]->id)
                        for (const live::SessionClip& x : t.clips)
                            if (x.slot == sc) c = &x;
                if (c == nullptr) {
                    g.setColour(colour::rowDivider);
                    g.drawRect(box, ui::theme::stroke::rule);
                    continue;
                }
                clipBlock(g, box, c->colour, juce::String(c->name), true);
                if (clip == c) {
                    g.setColour(colour::chosen);
                    g.drawRect(box, ui::theme::stroke::toggle);
                }
                //  the clip Live plays: what is now, ringed in the ground over the clip's fill
                const auto seen = playing_.find(tracks[i]->id);
                if (seen != playing_.end() && seen->second.report.slot == sc) {
                    const auto mark = juce::Rectangle<float>(tl::slotMark, tl::slotMark)
                                          .withCentre({box.getRight() - tl::slotMark, box.getCentreY()});
                    g.setColour(colour::ground);
                    g.fillRect(mark.expanded(ui::theme::stroke::rule));
                    g.setColour(colour::playhead);
                    g.fillRect(mark);
                }
                addClickArea(clickable, [this, id = tracks[i]->id, sc] {
                    chosenSlot_[id] = sc;
                    chosen_ = id;  // the panel and the pane follow the clip chosen
                    closeMenu();
                    repaint();
                });
            }
            y += tl::sceneRow;
        }
        noteContentBottom(y + scroll());
    }
    clipPane(g, {0.0f, paneTop_, right, h - paneTop_}, at, clip);
}

//  The chosen clip's pane: its name and track, a ruler in the clip's own bars from its loop's start to its end, and
//  its clip lanes, kPaneLanes at most in view, scrolled a lane at a time; where Live is in the clip -- run on while
//  Live plays, standing where Live last said while it stops.
void TimelinePage::clipPane(juce::Graphics& g, juce::Rectangle<float> area, const live::Playhead& at,
                            const live::SessionClip* clip) {
    g.setColour(colour::ground);
    g.fillRect(area);
    ui::ruleH(g, area.getX(), area.getRight(), area.getY(), colour::ruleStrong);
    const float top = area.getY();
    if (clip == nullptr) {
        ui::text(g, "choose a clip: its lanes open here", ui::labelFont(), colour::label,
                 {tl::pad, top, area.getWidth(), tl::ruler}, juce::Justification::centredLeft);
        return;
    }
    juce::String track;
    for (const Node& n : s_.tree.nodes)
        if (n.id == *chosen_) track = juce::String(n.name);
    ui::text(g, juce::String(clip->name) + ui::kSeparator + track, ui::labelFont(), colour::text,
             {tl::pad, top, tl::tree - tl::pad, tl::ruler}, juce::Justification::centredLeft);
    //  a clip's loop at least a sixteenth long, so an empty one still has an axis
    const double start = clip->loopStart, length = std::max(0.25, clip->loopEnd - clip->loopStart);
    const Axis axis{left_, width_, start, length, {clip->loopStart, clip->loopEnd}};
    for (int beat = 0; beat <= static_cast<int>(length); ++beat) {
        const float x = axis.x(start + beat);
        const bool bar = beat % live::kBeatsPerBar == 0;
        g.setColour(colour::rule);
        g.fillRect(x, top + tl::ruler - (bar ? tl::ruler / 2.0f : tl::ruler / 4.0f), 1.0f,
                   bar ? tl::ruler / 2.0f : tl::ruler / 4.0f);
        if (bar)
            ui::text(g, juce::String(beat / live::kBeatsPerBar + 1), ui::smallFont(), colour::label,
                     {x + tl::gap, top + tl::loopBrace, tl::barLabel, tl::ruler / 2.0f},
                     juce::Justification::centredLeft);
    }
    if (clip->looping) {
        g.setColour(colour::text);
        g.fillRect(axis.left, top, axis.width, tl::loopBrace);
    }
    ui::ruleH(g, area.getX(), area.getRight(), top + tl::ruler - 1.0f, colour::rule);
    //  its lanes, from the first in view
    float y = top + tl::ruler;
    int index = 0;
    for (live::Lane& l : s_.lanes) {
        if (!(l.clip() && l.clipTrack == *chosen_ && l.slot == clip->slot)) continue;
        if (index++ < paneScroll_ || y + tl::lane > area.getBottom() + 0.5f) continue;
        y = lane(g, l, y, axis);
    }
    if (paneLanes_ == 0)
        ui::text(g, "no lanes yet" + ui::kSeparator + "+ lane in the panel", ui::smallFont(), colour::label,
                 {tl::pad, y, area.getWidth(), tl::lane}, juce::Justification::centredLeft);
    else if (paneLanes_ > tl::paneLanes)
        ui::text(g,
                 juce::String(paneScroll_ + 1) + kDash + juce::String(paneScroll_ + tl::paneLanes) + " of " +
                     juce::String(paneLanes_),
                 ui::smallFont(), colour::label,
                 {tl::pad, top + tl::ruler / 2.0f, tl::tree - tl::pad, tl::ruler / 2.0f},
                 juce::Justification::centredLeft);
    //  where Live is in it
    const auto seen = playing_.find(*chosen_);
    if (seen != playing_.end() && seen->second.report.slot == clip->slot) {
        const double elapsed =
            at.playing ? (juce::Time::getMillisecondCounterHiRes() - seen->second.atMs) / 1000.0 * at.bpm / 60.0 : 0.0;
        const float x = axis.x(live::clipPosition(seen->second.report, elapsed));
        g.setColour(colour::playhead);
        g.fillRect(x, top, 1.0f, area.getBottom() - top);
    }
}

void TimelinePage::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) {
    if (view_ == View::Session) {
        //  over the pane: its lanes, a lane at a time
        if (e.position.y >= paneTop_) {
            paneScroll_ =
                std::clamp(paneScroll_ + (wheel.deltaY < 0.0f ? 1 : -1), 0, std::max(0, paneLanes_ - tl::paneLanes));
            repaint();
            return;
        }
        //  sideways, or with shift: the grid's tracks
        const bool across = std::abs(wheel.deltaX) > std::abs(wheel.deltaY) || e.mods.isShiftDown();
        if (across) {
            const float delta = std::abs(wheel.deltaX) > std::abs(wheel.deltaY) ? wheel.deltaX : wheel.deltaY;
            sideways_ -= delta * tl::wheelStep;
            repaint();
            return;
        }
    }
    if (view_ == View::Arrangement && e.position.x >= left_) {
        //  command: zoom about the pointer; sideways, or with shift: along the song
        if (e.mods.isCommandDown()) {
            time_ = live::zoomed(time_, std::exp(-static_cast<double>(wheel.deltaY) * tl::zoomRate),
                                 ppqOf(e.position.x), length_);
            repaint();
            return;
        }
        const bool across = std::abs(wheel.deltaX) > std::abs(wheel.deltaY) || e.mods.isShiftDown();
        if (across) {
            const float delta = std::abs(wheel.deltaX) > std::abs(wheel.deltaY) ? wheel.deltaX : wheel.deltaY;
            time_ = live::scrolled(time_, -static_cast<double>(delta) * tl::wheelStep / width_ * time_.span, length_);
            repaint();
            return;
        }
    }
    HitArea::mouseWheelMove(e, wheel);
}

void TimelinePage::mouseMagnify(const juce::MouseEvent& e, float scale) {
    if (view_ != View::Arrangement || scale <= 0.0f) return;
    //  a pinch: spread zooms in, about the pointer
    time_ = live::zoomed(time_, 1.0 / static_cast<double>(scale), ppqOf(e.position.x), length_);
    repaint();
}

float TimelinePage::lane(juce::Graphics& g, live::Lane& l, float y, const Axis& axis) {
    const auto owner = ownerOf(l);
    const ParamManifest* m = owner ? &manifestOf(owner->instance, owner->effect) : nullptr;
    const int at = m != nullptr ? m->byKey(l.param) : kNoParam;
    const juce::Rectangle<float> strip{0.0f, y, axis.left + axis.width, tl::lane};
    g.setColour(colour::rowDivider);
    g.fillRect(strip.getX(), strip.getBottom() - 1.0f, strip.getWidth() + tl::pad, 1.0f);
    ui::text(g, laneName(l), ui::smallFont(), colour::label,
             {tl::pad + tl::indent, y, tl::tree - tl::pad - tl::indent, tl::lane}, juce::Justification::centredLeft);
    if (m == nullptr || at < 0) return y + tl::lane;
    //  the curve and its points inside the time the axis shows, never over the names' column
    juce::Graphics::ScopedSaveState keep(g);
    g.reduceClipRegion(juce::Rectangle<float>{axis.left, y, axis.width, tl::lane}.toNearestInt());

    const bool live = !plays || plays(l);
    const juce::Colour ink = live ? colour::text : colour::inactive;
    const float top = y + tl::laneInset, height = tl::lane - 2.0f * tl::laneInset;
    const auto yOf = [&](double value) {
        return top + (1.0f - toNormalised(*m, at, static_cast<float>(value))) * height;
    };
    const auto valueOf = [m, at, top, height](float py) {
        return static_cast<double>(
            clampToRange(*m, at, fromNormalised(*m, at, std::clamp(1.0f - (py - top) / height, 0.0f, 1.0f))));
    };
    //  the curve: straight between the points, held past the first and the last; a lane without any, its
    //  parameter's default, dashed: it drives nothing
    juce::Path curve;
    if (l.points.empty()) {
        g.setColour(colour::inactive);
        curve.startNewSubPath(axis.left, yOf((*m)[at].def));
        curve.lineTo(axis.left + axis.width, yOf((*m)[at].def));
        ui::strokeDashed(g, curve, ui::theme::stroke::outsideDash, juce::PathStrokeType(ui::theme::stroke::rule));
    } else {
        curve.startNewSubPath(axis.left, yOf(l.points.front().value));
        for (const live::LanePoint& p : l.points) curve.lineTo(axis.x(p.ppq), yOf(p.value));
        curve.lineTo(axis.left + axis.width, yOf(l.points.back().value));
        g.setColour(ink);
        g.strokePath(curve, juce::PathStrokeType(ui::theme::stroke::glyph));
        //  the points in the handle ink, as every point that is grabbed is, the one held larger
        for (std::size_t i = 0; i < l.points.size(); ++i) {
            const juce::Point<float> c{axis.x(l.points[i].ppq), yOf(l.points[i].value)};
            const bool held = dragLane_ == l.id && dragPoint_ == i;
            const float r = ui::theme::controls::envelopePoint + (held ? ui::theme::scene::handleHotGrow : 0.0f);
            g.setColour(live ? colour::handle : colour::inactive);
            g.fillEllipse(juce::Rectangle<float>(2.0f * r, 2.0f * r).withCentre(c));
            if (!held) continue;
            //  where it is and what it sets, beside it -- on its left near the panel
            const juce::String readout = juce::String(live::barsBeats(l.points[i].ppq)) + ui::kSeparator +
                                         juce::String(formatParameter((*m)[at], l.points[i].value));
            const float rw = ui::textWidth(ui::smallFont(), readout) + tl::gap;
            const bool left = c.x + r + tl::gap + rw > axis.left + axis.width;
            ui::text(g, readout, ui::smallFont(), colour::text,
                     {left ? c.x - r - tl::gap - rw : c.x + r + tl::gap, y, rw, tl::lane / 2.0f},
                     left ? juce::Justification::centredRight : juce::Justification::centredLeft);
        }
    }

    //  a press puts a point, or takes the one under it; a drag moves it; a double click removes it
    const double pixelsPerBeat = static_cast<double>(axis.width) / axis.length;
    const double reach = static_cast<double>(tl::reach) / pixelsPerBeat;
    const double valueReach = std::abs(valueOf(top) - valueOf(top + tl::reach));  // the reach, in its units
    const std::vector<double> edges = axis.edges;
    const double grid = live::gridFor(pixelsPerBeat);
    const auto snapped = [axis, edges, grid, reach](float x, bool fine) {
        const double ppq = std::max(0.0, axis.at(x));
        return fine ? ppq : live::snapAt(ppq, edges, grid, reach);
    };
    const std::uint64_t id = l.id;
    const auto find = [this, id]() -> live::Lane* {
        for (live::Lane& x : s_.lanes)
            if (x.id == id) return &x;
        return nullptr;
    };
    //  a press takes the point under it; a click on empty ground puts one there, a drag from it puts one and moves
    //  it; a double click removes the one under it
    const auto take = [this, axis, find, valueOf, reach, valueReach, id](juce::Point<float> p) {
        live::Lane* lane = find();
        if (lane == nullptr) return;
        pressed_ = p;
        tookPoint_ = false;
        if (const auto near = live::pointNear(*lane, axis.at(p.x), valueOf(p.y), reach, valueReach)) {
            dragLane_ = id;
            dragPoint_ = *near;
            tookPoint_ = true;
            repaint();
        }
    };
    const auto move = [this, find, valueOf, snapped, id](juce::Point<float> p, bool fine) {
        live::Lane* lane = find();
        if (lane == nullptr) return;
        if (dragLane_ == 0) {
            dragLane_ = id;
            dragPoint_ = live::putPoint(*lane, {snapped(pressed_.x, false), valueOf(pressed_.y)});
        }
        dragPoint_ = live::movePoint(*lane, dragPoint_, {snapped(p.x, fine), valueOf(p.y)});
        repaint();
    };
    const auto put = [this, find, valueOf, snapped] {
        live::Lane* lane = find();
        if (lane == nullptr || tookPoint_) return;
        live::putPoint(*lane, {snapped(pressed_.x, false), valueOf(pressed_.y)});
        changed();
    };
    const auto remove = [this, axis, find, valueOf, reach, valueReach] {
        live::Lane* lane = find();
        if (lane == nullptr) return;
        if (const auto near = live::pointNear(*lane, axis.at(pressed_.x), valueOf(pressed_.y), reach, valueReach)) {
            live::removePoint(*lane, *near);
            changed();
        }
    };
    addGestureArea(
        juce::Rectangle<float>{axis.left, y, axis.width, tl::lane}, take, move,
        [this] {
            if (dragLane_ == 0) return;
            dragLane_ = 0;
            changed();
        },
        put, remove);
    return y + tl::lane;
}

void TimelinePage::panel(juce::Graphics& g, juce::Rectangle<float> area) {
    namespace ctl = ui::theme::controls;
    g.setColour(colour::ground);
    g.fillRect(area);
    g.setColour(colour::rule);
    g.fillRect(area.getX(), area.getY(), ui::theme::stroke::rule, area.getHeight());
    auto inner = area.reduced(tl::pad);
    addAt_ = {};
    if (!chosen_) {
        ui::text(g, "choose a track for its lanes", ui::labelFont(), colour::label,
                 inner.removeFromTop(ctl::listRowHeight), juce::Justification::centredLeft);
        return;
    }
    const live::TrackId row = *chosen_;
    juce::String title = "main";
    for (const Node& n : s_.tree.nodes)
        if (n.id == row) title = juce::String(n.name);
    float y = ui::drawGroupHeading(g, inner.getX(), inner.getY(), inner.getWidth(), title);
    //  its lanes, each removable after asking -- the host has no undo -- greyed while it plays nowhere
    const live::SessionClip* clip = view_ == View::Session ? chosenClip(row) : nullptr;
    for (const live::Lane& l : s_.lanes) {
        if (!belongs(l, row)) continue;
        if (view_ == View::Session ? !(l.clip() && clip != nullptr && l.slot == clip->slot && l.clipTrack == row)
                                   : l.clip())
            continue;
        const juce::Rectangle<float> r{inner.getX(), y, inner.getWidth(), ctl::listRowHeight};
        y += ctl::listRowHeight;
        const bool live = !plays || plays(l);
        ui::text(g, laneName(l), ui::smallFont(), live ? colour::text : colour::inactive, r.withTrimmedRight(tl::pad),
                 juce::Justification::centredLeft);
        const juce::Rectangle<float> cross{r.getRight() - ctl::closeGlyph, r.getCentreY() - ctl::closeGlyph / 2.0f,
                                           ctl::closeGlyph, ctl::closeGlyph};
        addClickArea(ui::drawCloseCross(g, cross), [this, id = l.id, name = laneName(l)] {
            confirm_.ask("remove the lane " + name + "?", "its points go with it; the host has no undo yet", "remove",
                         [this, id] {
                             s_.lanes.erase(std::remove_if(s_.lanes.begin(), s_.lanes.end(),
                                                           [&](const live::Lane& x) { return x.id == id; }),
                                            s_.lanes.end());
                             changed();
                         });
        });
    }
    if (ownedBy(row).empty() || (view_ == View::Session && clip == nullptr)) {
        ui::text(g,
                 view_ == View::Session && clip == nullptr ? "no session clip here" : "nothing of bambi's plays here",
                 ui::smallFont(), colour::label, {inner.getX(), y, inner.getWidth(), ctl::listRowHeight},
                 juce::Justification::centredLeft);
        return;
    }
    addAt_ = {inner.getX(), y + tl::gap, ui::framedButtonWidth("+ lane"), ui::theme::sheet::control};
    ui::drawFramedButton(g, addAt_, "+ lane", true, adding_);
    addClickArea(addAt_, [this] {
        adding_ = !adding_;
        addInstance_.reset();
        addGroup_.reset();
        grabKeyboardFocus();
        repaint();
    });
}

//  "+ lane"'s menu, over everything: the row's instances, then the chosen one's groups, then their parameters.
//  A press anywhere else closes it; escape steps back a level.
void TimelinePage::menu(juce::Graphics& g) {
    if (!adding_ || !chosen_ || addAt_.isEmpty()) return;
    const auto owned = ownedBy(*chosen_);
    std::vector<ui::MenuItem> items;
    std::vector<std::function<void()>> acts;
    juce::String foot = "a lane drives one parameter of one of these";
    if (!addInstance_ || *addInstance_ >= owned.size()) {
        for (std::size_t i = 0; i < owned.size(); ++i) {
            items.push_back({owned[i].name, {}, true});
            acts.push_back([this, i] {
                addInstance_ = i;
                repaint();
            });
        }
    } else {
        const Owned o = owned[*addInstance_];
        const ParamManifest& m = manifestOf(o.instance, o.effect);
        foot = o.name + ui::kSeparator + "esc steps back";
        if (!addGroup_) {
            std::vector<ParamGroup> groups;
            for (int i = 0; i < m.size(); ++i)
                if (std::find(groups.begin(), groups.end(), m[i].group) == groups.end()) groups.push_back(m[i].group);
            for (const ParamGroup group : groups) {
                items.push_back({groupName(group), {}, true});
                acts.push_back([this, group] {
                    addGroup_ = group;
                    repaint();
                });
            }
        } else {
            for (int i = 0; i < m.size(); ++i) {
                if (m[i].group != *addGroup_) continue;
                items.push_back({juce::String(std::string(m[i].name)), juce::String(std::string(m[i].unit)), true});
                acts.push_back([this, o, key = std::string(m[i].key)] {
                    std::uint64_t next = 1;
                    for (const live::Lane& l : s_.lanes) next = std::max(next, l.id + 1);
                    live::Lane lane{next, o.instance, o.owner, key,
                                    {},   0,          -1,      {}};  // a song lane until a clip is chosen
                    if (view_ == View::Session)
                        if (const live::SessionClip* clip = chosenClip(*chosen_)) {
                            lane.clipTrack = *chosen_;
                            lane.slot = clip->slot;
                            lane.clipName = clip->name;
                        }
                    s_.lanes.push_back(std::move(lane));
                    closeMenu();
                    changed();
                });
            }
        }
    }
    namespace sh = ui::theme::sheet;
    addClickArea(getLocalBounds().toFloat(), [this] {
        closeMenu();
        repaint();
    });
    const float height = 2.0f * sh::menuPad + sh::menuLine * static_cast<float>(items.size() + 1);
    const float x = std::min(addAt_.getX(), static_cast<float>(getWidth()) - sh::menuWidth - tl::pad);
    const float y = std::max(static_cast<float>(ui::theme::metrics::headerHeight),
                             std::min(addAt_.getBottom(), static_cast<float>(getHeight()) - height - tl::pad));
    const auto drawn = ui::drawMenu(g, {x, y}, sh::menuWidth, items, foot);
    addClickArea(drawn.box, [] {});
    for (std::size_t i = 0; i < drawn.items.size(); ++i) addClickArea(drawn.items[i], acts[i]);
}

void TimelinePage::ruler(juce::Graphics& g, float top) {
    const juce::Rectangle<float> strip{left_, top, width_, tl::ruler};
    juce::Graphics::ScopedSaveState keep(g);
    g.reduceClipRegion(strip.toNearestInt());
    //  the loop, a band along the top: ink while it is on
    if (s_.arrangement.loopLength > 0.0) {
        g.setColour(s_.arrangement.loopOn ? colour::text : colour::inactive);
        const float x0 = xOf(s_.arrangement.loopStart), x1 = xOf(s_.arrangement.loopStart + s_.arrangement.loopLength);
        g.fillRect(x0, top, x1 - x0, tl::loopBrace);
    }
    //  the bars, numbered as room allows
    const float barWidth = width_ * static_cast<float>(live::kBeatsPerBar / time_.span);
    int every = 1;
    while (barWidth * static_cast<float>(every) < tl::barLabel) every *= 2;
    const auto [firstBar, lastBar] = barsIn(time_);
    for (int bar = std::max(0, firstBar); bar <= lastBar; ++bar) {
        const float x = xOf(bar * live::kBeatsPerBar);
        const bool numbered = bar % every == 0;
        g.setColour(colour::rule);
        g.fillRect(x, top + tl::ruler - (numbered ? tl::ruler / 2.0f : tl::ruler / 4.0f), 1.0f,
                   numbered ? tl::ruler / 2.0f : tl::ruler / 4.0f);
        if (numbered)
            ui::text(g, juce::String(bar + 1), ui::smallFont(), colour::label,
                     {x + tl::gap, top + tl::loopBrace, tl::barLabel, tl::ruler / 2.0f},
                     juce::Justification::centredLeft);
    }
    //  the locators: a mark and a name
    for (const live::Locator& l : s_.arrangement.locators) {
        const float x = xOf(l.time);
        juce::Path mark;
        mark.addTriangle(x, top + tl::ruler - 2.0f * tl::locator, x + tl::locator, top + tl::ruler - tl::locator, x,
                         top + tl::ruler);
        g.setColour(colour::text);
        g.fillPath(mark);
        ui::text(g, juce::String(l.name), ui::smallFont(), colour::text,
                 {x + tl::locator + tl::gap, top + tl::ruler / 2.0f, tl::tree, tl::ruler / 2.0f},
                 juce::Justification::centredLeft);
    }
    //  a click locates: to a locator near it, else to the beat
    addGestureArea(
        strip, [this](juce::Point<float> at) { pressedAt_ = at.x; }, [](juce::Point<float>, bool) {}, [] {},
        [this] {
            const double reach = static_cast<double>(tl::reach / width_) * time_.span;
            if (command)
                command({live::Command::Action::Locate, live::locateAt(ppqOf(pressedAt_), s_.arrangement, 1.0, reach)});
        },
        [] {});
}

void TimelinePage::band(juce::Graphics& g, const live::Playhead& at) {
    const auto inner = ui::drawPageBand(g, static_cast<float>(getWidth()));
    const float h = inner.getHeight();
    float x = inner.getX();
    //  Live's transport as one choice: play the chosen block while Live plays
    const std::vector<juce::String> transport{"play", "stop"};
    const juce::Rectangle<float> box{x, (h - ui::theme::sheet::control) / 2.0f, ui::segmentsWidth(transport),
                                     ui::theme::sheet::control};
    const auto cells = ui::drawSegments(g, box, transport, at.playing ? 0 : 1, true);
    addClickArea(cells[0], [this] {
        if (command) command({live::Command::Action::Continue, 0.0});
    });
    addClickArea(cells[1], [this] {
        if (command) command({live::Command::Action::Stop, 0.0});
    });
    x = box.getRight() + tl::pad;
    const auto valueFont = ui::font(ui::theme::type::meterValue);
    const float positionWidth = ui::textWidth(valueFont, "000.0.0") + tl::gap;
    ui::text(g, juce::String(live::barsBeats(at.ppq)), valueFont, colour::text, {x, 0.0f, positionWidth, h},
             juce::Justification::centredLeft);
    x += positionWidth + tl::pad;
    const juce::String tempo = juce::String(at.bpm, 2) + " bpm";
    ui::text(g, tempo, ui::labelFont(), colour::label, {x, 0.0f, ui::textWidth(ui::labelFont(), tempo) + tl::gap, h},
             juce::Justification::centredLeft);
    x += ui::textWidth(ui::labelFont(), tempo) + tl::gap + tl::pad;
    const std::vector<juce::String> views{"arrangement", "session"};
    const juce::Rectangle<float> switcher{x, (h - ui::theme::sheet::control) / 2.0f, ui::segmentsWidth(views),
                                          ui::theme::sheet::control};
    const auto viewCells = ui::drawSegments(g, switcher, views, view_ == View::Arrangement ? 0 : 1, true);
    for (std::size_t i = 0; i < viewCells.size(); ++i)
        addClickArea(viewCells[i], [this, i] {
            view_ = i == 0 ? View::Arrangement : View::Session;
            closeMenu();
            if (refresh) refresh();
            repaint();
        });
    if (s_.arrangement.readMs > 0.0) {
        const juce::String read = "live gave its arrangement in " + juce::String(s_.arrangement.readMs, 0) + " ms";
        const float rw = ui::textWidth(ui::smallFont(), read) + tl::gap;
        ui::text(g, read, ui::smallFont(), colour::label, {inner.getRight() - rw, 0.0f, rw, h},
                 juce::Justification::centredLeft);
    }
}

}  // namespace bambi::livehost
