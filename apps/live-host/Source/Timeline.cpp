// SPDX-License-Identifier: GPL-3.0-or-later
#include "Timeline.h"

#include <algorithm>
#include <cmath>
#include <vector>

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

}  // namespace

TimelinePage::TimelinePage() { startTimer(tl::frameMs); }

void TimelinePage::show(const live::Tree& tree, const live::Arrangement& arrangement) {
    tree_ = tree;
    arrangement_ = arrangement;
    length_ = live::songLength(arrangement_);
    repaint();
}

void TimelinePage::timerCallback() {
    //  while Live plays, and once more as it stops, so the playhead rests where it stopped
    const bool playing = isShowing() && heard && heard().playing;
    if (playing || wasPlaying_) repaint();
    wasPlaying_ = playing;
}

float TimelinePage::xOf(double ppq) const { return left_ + static_cast<float>(ppq / length_) * width_; }
double TimelinePage::ppqOf(float x) const { return static_cast<double>((x - left_) / width_) * length_; }

void TimelinePage::paint(juce::Graphics& g) {
    g.fillAll(colour::ground);
    clearRegions();
    const auto w = static_cast<float>(getWidth());
    const auto top = static_cast<float>(ui::theme::metrics::headerHeight);
    left_ = tl::tree;
    width_ = std::max(1.0f, w - tl::tree - tl::pad);
    const live::Playhead at = heard ? heard() : live::Playhead{};

    ruler(g, top);
    const float rows = top + tl::ruler;
    setViewport(rows, static_cast<float>(getHeight()));
    {
        const ContentClip clip(*this, g);
        float y = rows - scroll();
        //  the bars behind everything
        for (int bar = 0; bar * live::kBeatsPerBar <= length_; ++bar) {
            const float x = xOf(bar * live::kBeatsPerBar);
            g.setColour(bar % 4 == 0 ? colour::rule : colour::rowDivider);
            g.fillRect(x, rows, 1.0f, static_cast<float>(getHeight()) - rows);
        }
        bool returns = false;
        for (const Node& n : tree_.nodes) {
            if (n.kind == Node::Kind::Return && !returns) {
                returns = true;
                y = ui::drawGroupHeading(g, tl::pad, y + tl::pad, w - 2.0f * tl::pad, "returns");
            }
            const juce::Rectangle<float> row{0.0f, y, w, tl::row};
            g.setColour(colour::rowDivider);
            g.fillRect(row.getX(), row.getBottom() - 1.0f, row.getWidth(), 1.0f);
            //  the header: the node's name, indented by its depth, a group or a return said
            const float indent = tl::pad + tl::indent * static_cast<float>(depthOf(tree_, n));
            const juce::Rectangle<float> head{indent, y, tl::tree - indent - tl::pad, tl::row};
            ui::text(g, juce::String(n.name), ui::labelFont(), colour::text, head, juce::Justification::centredLeft);
            if (n.kind != Node::Kind::Track)
                ui::text(g, n.kind == Node::Kind::Group ? "group" : "return", ui::smallFont(), colour::label, head,
                         juce::Justification::centredRight);
            //  its clips, as Live has them: read-only
            const auto clips = std::find_if(arrangement_.tracks.begin(), arrangement_.tracks.end(),
                                            [&](const live::TrackClips& t) { return t.id == n.id; });
            if (clips != arrangement_.tracks.end())
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
        }
        noteContentBottom(y + scroll());
        //  the playhead, over the rows
        g.setColour(colour::playhead);
        g.fillRect(xOf(at.ppq), rows, 1.0f, static_cast<float>(getHeight()) - rows);
    }
    g.setColour(colour::playhead);
    g.fillRect(xOf(at.ppq), top, 1.0f, tl::ruler);
    band(g, at);
}

void TimelinePage::ruler(juce::Graphics& g, float top) {
    const auto w = static_cast<float>(getWidth());
    const juce::Rectangle<float> strip{0.0f, top, w, tl::ruler};
    //  the loop, a band along the top: ink while it is on
    if (arrangement_.loopLength > 0.0) {
        g.setColour(arrangement_.loopOn ? colour::text : colour::inactive);
        const float x0 = xOf(arrangement_.loopStart), x1 = xOf(arrangement_.loopStart + arrangement_.loopLength);
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
    for (const live::Locator& l : arrangement_.locators) {
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
        strip.withTrimmedLeft(tl::tree - 1.0f), [this](juce::Point<float> at) { pressedAt_ = at.x; },
        [](juce::Point<float>, bool) {}, [] {},
        [this] {
            const double reach = static_cast<double>(tl::reach / width_) * length_;
            if (command)
                command({live::Command::Action::Locate, live::locateAt(ppqOf(pressedAt_), arrangement_, 1.0, reach)});
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
    if (arrangement_.readMs > 0.0) {
        const juce::String read = "live gave its arrangement in " + juce::String(arrangement_.readMs, 0) + " ms";
        const float rw = ui::textWidth(ui::smallFont(), read) + tl::gap;
        ui::text(g, read, ui::smallFont(), colour::label, {inner.getRight() - rw, 0.0f, rw, h},
                 juce::Justification::centredLeft);
    }
}

}  // namespace bambi::livehost
