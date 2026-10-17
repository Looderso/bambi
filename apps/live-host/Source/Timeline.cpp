// SPDX-License-Identifier: GPL-3.0-or-later
#include "Timeline.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "Plugins.h"
#include "bambi/live/timeline.hpp"
#include "bambi/ui/Draw.h"
#include "bambi/ui/Widgets.h"

namespace bambi::livehost {

namespace {

namespace colour = ui::theme::colour;
namespace tl = ui::theme::timeline;
using live::Node;

//  How deep a node sits: 0 at the top, one more for each group it plays into.
int depthOf(const live::Tree& tree, const Node& n) {
    int depth = 0;
    const Node* at = &n;
    while (at->parent && depth < 64) {
        const auto up = std::find_if(tree.nodes.begin(), tree.nodes.end(),
                                     [&](const Node& x) { return x.id == *at->parent; });
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
    repaint();
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

void TimelinePage::timerCallback() {
    //  while Live plays, and once more as it stops, so the playhead rests where it stopped
    const bool playing = isShowing() && heard && heard().playing;
    if (playing || wasPlaying_) repaint();
    wasPlaying_ = playing;
}

float TimelinePage::xOf(double ppq) const { return left_ + static_cast<float>(ppq / length_) * width_; }
double TimelinePage::ppqOf(float x) const { return static_cast<double>((x - left_) / width_) * length_; }

std::vector<TimelinePage::Owned> TimelinePage::ownedBy(live::TrackId row) const {
    std::vector<Owned> out;
    if (row != kMainRow) {
        const auto sources = s_.field.sources();
        if (std::find(sources.begin(), sources.end(), row) != sources.end())
            out.push_back({live::Instance::Encoder, row, live::Effect::Echo, "encoder"});
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
    if (l.instance == live::Instance::Encoder) return Owned{l.instance, l.owner, live::Effect::Echo, "encoder"};
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

    ruler(g, top);
    const float rows = top + tl::ruler;
    const float right = left_ + width_ + tl::pad;
    setViewport(rows, static_cast<float>(getHeight()));
    {
        const ContentClip clip(*this, g);
        float y = rows - scroll();
        for (int bar = 0; bar * live::kBeatsPerBar <= length_; ++bar) {
            const float x = xOf(bar * live::kBeatsPerBar);
            g.setColour(bar % 4 == 0 ? colour::rule : colour::rowDivider);
            g.fillRect(x, rows, 1.0f, static_cast<float>(getHeight()) - rows);
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
                adding_ = false;
                addInstance_.reset();
                addGroup_.reset();
                grabKeyboardFocus();
                repaint();
            });
        };
        const auto lanesOf = [&](live::TrackId id) {
            for (live::Lane& l : s_.lanes)
                if (belongs(l, id)) y = lane(g, l, y, id);
        };
        bool returns = false;
        for (const Node& n : s_.tree.nodes) {
            if (n.kind == Node::Kind::Return && !returns) {
                returns = true;
                y = ui::drawGroupHeading(g, tl::pad, y + tl::pad, right - tl::pad, "returns");
            }
            header(n.id, juce::String(n.name), depthOf(s_.tree, n),
                   n.kind == Node::Kind::Group ? "group" : n.kind == Node::Kind::Return ? "return" : "");
            //  its clips, as Live has them: read-only
            const auto clips = std::find_if(s_.arrangement.tracks.begin(), s_.arrangement.tracks.end(),
                                            [&](const live::TrackClips& t) { return t.id == n.id; });
            if (clips != s_.arrangement.tracks.end())
                for (const live::Clip& c : clips->clips) {
                    const float x0 = xOf(c.start), x1 = xOf(c.end);
                    const juce::Rectangle<float> block{x0, y + tl::clipInset, std::max(1.0f, x1 - x0 - 1.0f),
                                                       tl::row - 2.0f * tl::clipInset};
                    const juce::Colour ink = colour::ofLive(juce::String(c.colour), colour::label);
                    g.setColour(ink.withAlpha(tl::clipFill));
                    g.fillRect(block);
                    g.setColour(ink);
                    g.fillRect(block.withHeight(tl::clipBar));
                    if (block.getWidth() > tl::barLabel)
                        ui::text(g, juce::String(c.name), ui::smallFont(), colour::text,
                                 block.reduced(tl::clipInset, 0.0f).withTrimmedTop(tl::clipBar),
                                 juce::Justification::topLeft);
                }
            y += tl::row;
            lanesOf(n.id);
        }
        y = ui::drawGroupHeading(g, tl::pad, y + tl::pad, right - tl::pad, "main");
        header(kMainRow, "main", 0, "chain" + ui::kSeparator + "decoders");
        y += tl::row;
        lanesOf(kMainRow);
        noteContentBottom(y + scroll());
        g.setColour(colour::playhead);
        g.fillRect(xOf(at.ppq), rows, 1.0f, static_cast<float>(getHeight()) - rows);
    }
    g.setColour(colour::playhead);
    g.fillRect(xOf(at.ppq), top, 1.0f, tl::ruler);
    panel(g, {right, rows, w - right, static_cast<float>(getHeight()) - rows});
    band(g, at);
    menu(g);
}

float TimelinePage::lane(juce::Graphics& g, live::Lane& l, float y, live::TrackId row) {
    const auto owner = ownerOf(l);
    const ParamManifest* m = owner ? &manifestOf(owner->instance, owner->effect) : nullptr;
    const int at = m != nullptr ? m->byKey(l.param) : kNoParam;
    const juce::Rectangle<float> strip{0.0f, y, left_ + width_, tl::lane};
    g.setColour(colour::rowDivider);
    g.fillRect(strip.getX(), strip.getBottom() - 1.0f, strip.getWidth() + tl::pad, 1.0f);
    ui::text(g, laneName(l), ui::smallFont(), colour::label,
             {tl::pad + tl::indent, y, tl::tree - tl::pad - tl::indent, tl::lane}, juce::Justification::centredLeft);
    if (m == nullptr || at < 0) return y + tl::lane;

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
        curve.startNewSubPath(left_, yOf((*m)[at].def));
        curve.lineTo(left_ + width_, yOf((*m)[at].def));
        ui::strokeDashed(g, curve, ui::theme::stroke::outsideDash, juce::PathStrokeType(ui::theme::stroke::rule));
    } else {
        curve.startNewSubPath(left_, yOf(l.points.front().value));
        for (const live::LanePoint& p : l.points) curve.lineTo(xOf(p.ppq), yOf(p.value));
        curve.lineTo(left_ + width_, yOf(l.points.back().value));
        g.setColour(ink);
        g.strokePath(curve, juce::PathStrokeType(ui::theme::stroke::glyph));
        //  the points in the handle ink, as every point that is grabbed is, the one held larger
        for (std::size_t i = 0; i < l.points.size(); ++i) {
            const juce::Point<float> c{xOf(l.points[i].ppq), yOf(l.points[i].value)};
            const bool held = dragLane_ == l.id && dragPoint_ == i;
            const float r = ui::theme::controls::envelopePoint + (held ? ui::theme::scene::handleHotGrow : 0.0f);
            g.setColour(live ? colour::handle : colour::inactive);
            g.fillEllipse(juce::Rectangle<float>(2.0f * r, 2.0f * r).withCentre(c));
            if (!held) continue;
            //  where it is and what it sets, beside it -- on its left near the panel
            const juce::String readout = juce::String(live::barsBeats(l.points[i].ppq)) + ui::kSeparator +
                                         juce::String(formatParameter((*m)[at], l.points[i].value));
            const float rw = ui::textWidth(ui::smallFont(), readout) + tl::gap;
            const bool left = c.x + r + tl::gap + rw > left_ + width_;
            ui::text(g, readout, ui::smallFont(), colour::text,
                     {left ? c.x - r - tl::gap - rw : c.x + r + tl::gap, y, rw, tl::lane / 2.0f},
                     left ? juce::Justification::centredRight : juce::Justification::centredLeft);
        }
    }

    //  a press puts a point, or takes the one under it; a drag moves it; a double click removes it
    const double pixelsPerBeat = static_cast<double>(width_) / length_;
    const double reach = static_cast<double>(tl::reach) / pixelsPerBeat;
    const double valueReach = std::abs(valueOf(top) - valueOf(top + tl::reach));  // the reach, in its units
    const std::vector<double> edges = live::edgesOf(s_.arrangement, row);
    const double grid = live::gridFor(pixelsPerBeat);
    const auto snapped = [this, edges, grid, reach](float x, bool fine) {
        const double ppq = std::max(0.0, ppqOf(x));
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
    const auto take = [this, find, valueOf, reach, valueReach, id](juce::Point<float> p) {
        live::Lane* lane = find();
        if (lane == nullptr) return;
        pressed_ = p;
        tookPoint_ = false;
        if (const auto near = live::pointNear(*lane, ppqOf(p.x), valueOf(p.y), reach, valueReach)) {
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
    const auto remove = [this, find, valueOf, reach, valueReach] {
        live::Lane* lane = find();
        if (lane == nullptr) return;
        if (const auto near = live::pointNear(*lane, ppqOf(pressed_.x), valueOf(pressed_.y), reach, valueReach)) {
            live::removePoint(*lane, *near);
            changed();
        }
    };
    addGestureArea(
        juce::Rectangle<float>{left_, y, width_, tl::lane}, take, move,
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
    for (const live::Lane& l : s_.lanes) {
        if (!belongs(l, row)) continue;
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
    if (ownedBy(row).empty()) {
        ui::text(g, "nothing of bambi's plays here", ui::smallFont(), colour::label,
                 {inner.getX(), y, inner.getWidth(), ctl::listRowHeight}, juce::Justification::centredLeft);
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
                    s_.lanes.push_back({next, o.instance, o.owner, key, {}});
                    adding_ = false;
                    addInstance_.reset();
                    addGroup_.reset();
                    changed();
                });
            }
        }
    }
    namespace sh = ui::theme::sheet;
    addClickArea(getLocalBounds().toFloat(), [this] {
        adding_ = false;
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
    const auto w = static_cast<float>(getWidth());
    const juce::Rectangle<float> strip{tl::tree - 1.0f, top, width_ + 2.0f, tl::ruler};
    //  the loop, a band along the top: ink while it is on
    if (s_.arrangement.loopLength > 0.0) {
        g.setColour(s_.arrangement.loopOn ? colour::text : colour::inactive);
        const float x0 = xOf(s_.arrangement.loopStart),
                    x1 = xOf(s_.arrangement.loopStart + s_.arrangement.loopLength);
        g.fillRect(x0, top, x1 - x0, tl::loopBrace);
    }
    //  the bars, numbered as room allows
    const float barWidth = width_ * static_cast<float>(live::kBeatsPerBar / length_);
    int every = 1;
    while (barWidth * static_cast<float>(every) < tl::barLabel) every *= 2;
    for (int bar = 0; bar * live::kBeatsPerBar <= length_; ++bar) {
        const float x = xOf(bar * live::kBeatsPerBar);
        const bool numbered = bar % every == 0;
        g.setColour(colour::rule);
        g.fillRect(x, top + tl::ruler - (numbered ? tl::ruler / 2.0f : tl::ruler / 4.0f), 1.0f,
                   numbered ? tl::ruler / 2.0f : tl::ruler / 4.0f);
        if (numbered)
            ui::text(g, juce::String(bar + 1), ui::smallFont(), colour::label,
                     {x + tl::gap, top + tl::loopBrace, tl::barLabel, tl::ruler / 2.0f}, juce::Justification::centredLeft);
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
    ui::ruleH(g, 0.0f, w, top + tl::ruler - 1.0f, colour::rule);
    //  a click locates: to a locator near it, else to the beat
    addGestureArea(
        strip, [this](juce::Point<float> at) { pressedAt_ = at.x; }, [](juce::Point<float>, bool) {}, [] {},
        [this] {
            const double reach = static_cast<double>(tl::reach / width_) * length_;
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
    if (s_.arrangement.readMs > 0.0) {
        const juce::String read = "live gave its arrangement in " + juce::String(s_.arrangement.readMs, 0) + " ms";
        const float rw = ui::textWidth(ui::smallFont(), read) + tl::gap;
        ui::text(g, read, ui::smallFont(), colour::label, {inner.getRight() - rw, 0.0f, rw, h},
                 juce::Justification::centredLeft);
    }
}

}  // namespace bambi::livehost
