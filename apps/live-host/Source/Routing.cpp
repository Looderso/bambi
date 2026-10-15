// SPDX-License-Identifier: GPL-3.0-or-later
#include "Routing.h"

#include <algorithm>
#include <cmath>

#include "bambi/ui/Draw.h"
#include "bambi/ui/Widgets.h"

namespace bambi::livehost {

namespace {

namespace colour = ui::theme::colour;
namespace rt = ui::theme::routing;
using live::Node;

const juce::String kDot = juce::String::fromUTF8(" \xc2\xb7 ");

juce::String nameOf(live::Effect e) { return e == live::Effect::Reverb ? "reverb" : "echo"; }

juce::Colour colourOf(live::Effect e) {
    return colour::identity(e == live::Effect::Reverb ? Product::Reverb : Product::Echo);
}

//  A frame, solid or dashed as what Live plays outside the field is.
void frame(juce::Graphics& g, juce::Rectangle<float> box, juce::Colour c, float weight, bool dashed) {
    g.setColour(c);
    if (!dashed) {
        g.drawRect(box, weight);
        return;
    }
    juce::Path p;
    p.addRectangle(box);
    juce::Path d;
    juce::PathStrokeType(weight).createDashedStroke(d, p, ui::theme::stroke::outsideDash, 2);
    g.fillPath(d);
}

//  The ring round a node under the pointer, outside its frame.
void hoverRing(juce::Graphics& g, juce::Rectangle<float> box) {
    g.setColour(colour::touched);
    g.drawRect(box.expanded(2.0f * ui::theme::stroke::rule), ui::theme::stroke::rule);
}

//  A connector: from a child's right edge to its parent's left edge, across and down.
void connect(juce::Graphics& g, juce::Point<float> from, juce::Point<float> to, bool field) {
    juce::Path p;
    const float mid = (from.x + to.x) / 2.0f;
    p.startNewSubPath(from);
    p.lineTo(mid, from.y);
    p.lineTo(mid, to.y);
    p.lineTo(to);
    g.setColour(field ? colour::ruleStrong : colour::rule);
    if (field) {
        g.strokePath(p, juce::PathStrokeType(ui::theme::stroke::rule));
        return;
    }
    juce::Path d;
    juce::PathStrokeType(ui::theme::stroke::rule).createDashedStroke(d, p, ui::theme::stroke::outsideDash, 2);
    g.fillPath(d);
}

//  A send's level as the row under a node says it.
juce::String dB(double v) {
    const double r = std::round(v);
    return std::abs(r) < 0.5
               ? juce::String("0")
               : (r < 0 ? juce::String::fromUTF8("\xe2\x88\x92") : "") + juce::String(static_cast<int>(std::abs(r)));
}

}  // namespace

RoutingPage::RoutingPage() {
    addChildComponent(confirm_);
    confirm_.answered = [this] { repaint(); };
}

void RoutingPage::show(const live::Tree& tree, const live::Shape& shape, const live::Field& field,
                       const std::vector<live::TrackPair>& pairs) {
    tree_ = tree;
    shape_ = shape;
    field_ = field;
    pairs_ = pairs;
    layout_ = live::layOutPage(tree_);
    repaint();
}

void RoutingPage::resized() { confirm_.setBounds(getLocalBounds()); }

const live::Part* RoutingPage::partOf(live::TrackId id) const {
    const auto p =
        std::find_if(field_.parts.begin(), field_.parts.end(), [&](const live::Part& x) { return x.node == id; });
    return p == field_.parts.end() ? nullptr : &*p;
}

int RoutingPage::pairOf(live::TrackId id) const {
    const auto p = std::find_if(pairs_.begin(), pairs_.end(), [&](const live::TrackPair& x) { return x.track == id; });
    return p == pairs_.end() ? -1 : p->pair;
}

void RoutingPage::change(const std::function<void(live::Shape&)>& edit) {
    live::Shape next = shape_;
    edit(next);
    if (reshape) reshape(std::move(next));
}

void RoutingPage::mouseMove(const juce::MouseEvent& e) {
    live::TrackId now = 0;
    for (const auto& [area, id] : rows_)
        if (area.contains(e.position)) now = id;
    if (now != hover_) {
        hover_ = now;
        repaint();
    }
}

void RoutingPage::mouseExit(const juce::MouseEvent&) {
    if (hover_ == 0) return;
    hover_ = 0;
    repaint();
}

void RoutingPage::paint(juce::Graphics& g) {
    g.fillAll(colour::ground);
    clearRegions();
    rows_.clear();
    const auto top = static_cast<float>(ui::theme::metrics::headerHeight);
    setViewport(top, static_cast<float>(getHeight()));
    {
        const ContentClip clip(*this, g);
        body(g, top + rt::top - scroll());
    }
    band(g);
}

void RoutingPage::band(juce::Graphics& g) {
    const auto w = static_cast<float>(getWidth());
    const auto h = static_cast<float>(ui::theme::metrics::headerHeight);
    g.setColour(colour::ground);
    g.fillRect(0.0f, 0.0f, w, h);
    const int sources = static_cast<int>(field_.sources().size());
    const int paired = static_cast<int>(pairs_.size());
    const juce::String count =
        juce::String(sources) + (sources == 1 ? " source" : " sources") + kDot + juce::String(paired) + " on pairs";
    const juce::String toMain = juce::String("field ") + juce::String::fromUTF8("\xe2\x86\x92") + " main";
    const auto f = ui::labelFont();
    ui::text(g, "live", f, colour::label, {rt::left, 0.0f, ui::textWidth(f, "live") + 2.0f, h});
    ui::text(g, count, f, colour::label, {rt::left + rt::boxWidth, 0.0f, ui::textWidth(f, count) + 2.0f, h});
    const float tw = ui::textWidth(f, toMain) + 2.0f;
    ui::text(g, toMain, f, colour::label, {w - rt::left - tw, 0.0f, tw, h});
    g.setColour(colour::rule);
    g.fillRect(0.0f, h - ui::theme::stroke::identityRule, w, ui::theme::stroke::identityRule);
}

void RoutingPage::body(juce::Graphics& g, float top) {
    const auto w = static_cast<float>(getWidth());
    //  each column as wide as its widest node with its controls; the gutters narrow for a deep set
    const int mainColumn = layout_.main.column;
    std::vector<float> widest(static_cast<std::size_t>(mainColumn), rt::boxWidth);
    for (const live::Place& p : layout_.places) {
        const auto n =
            std::find_if(tree_.nodes.begin(), tree_.nodes.end(), [&](const Node& x) { return x.id == p.node; });
        if (n != tree_.nodes.end() && p.column < mainColumn)
            widest[static_cast<std::size_t>(p.column)] =
                std::max(widest[static_cast<std::size_t>(p.column)], rt::boxWidth + controlsWidth(*n));
    }
    float span = 0.0f;
    for (const float c : widest) span += c;
    const float room = w - 2.0f * rt::left - rt::mainWidth - rt::chainRoom - span;
    const float gutter = std::clamp(room / static_cast<float>(std::max(1, mainColumn)), rt::gutterLeast, rt::gutter);
    std::vector<float> columnX(static_cast<std::size_t>(mainColumn) + 1, rt::left);
    for (int c = 0; c < mainColumn; ++c)
        columnX[static_cast<std::size_t>(c) + 1] =
            columnX[static_cast<std::size_t>(c)] + widest[static_cast<std::size_t>(c)] + gutter;
    const auto at = [&](const live::Place& p) {
        return juce::Point<float>{columnX[static_cast<std::size_t>(std::clamp(p.column, 0, mainColumn))],
                                  top + static_cast<float>(p.row) * rt::row + rt::boxHeight / 2.0f};
    };
    const auto placeOf = [&](live::TrackId id) -> const live::Place* {
        const auto p = std::find_if(layout_.places.begin(), layout_.places.end(),
                                    [&](const live::Place& x) { return x.node == id; });
        return p == layout_.places.end() ? nullptr : &*p;
    };

    //  the connectors first, under the boxes
    for (const Node& n : tree_.nodes) {
        const auto* p = placeOf(n.id);
        if (p == nullptr) continue;
        const live::Place* to = n.parent ? placeOf(*n.parent) : nullptr;
        //  from where the node's switch and chips end, so the line runs through none of them
        const auto from = at(*p) + juce::Point<float>{rt::boxWidth + controlsWidth(n), 0.0f};
        connect(g, from, at(to != nullptr ? *to : layout_.main), partOf(n.id) != nullptr);
    }
    for (const Node& n : tree_.nodes)
        if (const auto* p = placeOf(n.id)) node(g, n, at(*p));

    //  Main, its chain, the decoder
    {
        const auto m = at(layout_.main);
        const juce::Rectangle<float> box{m.x, m.y - rt::boxHeight / 2.0f, rt::mainWidth, rt::boxHeight};
        rows_.push_back(
            {{box.getX(), box.getY(), rt::mainWidth + chainWidth(live::kMain) + rt::gap * 3.0f, rt::row}, kHoverMain});
        frame(g, box, colour::ruleStrong, ui::theme::stroke::rule, false);
        if (hover_ == kHoverMain) hoverRing(g, box);
        ui::text(g, "main", ui::labelFont(), colour::text, box.withTrimmedLeft(rt::padX));
        float x = chain(g, live::kMain, box.getRight() + rt::gap * 2.0f, m.y);
        if (hover_ == kHoverMain)
            adders(g, live::kMain, box.getX(), box.getBottom() + rt::control / 2.0f + rt::gap / 2.0f);
        //  the decoder, a node of its own after Main's chain, in its plugin's colour; a click shows it
        const float dx = std::min(x + rt::gutterLeast, w - rt::left - rt::mainWidth);
        connect(g, {x - rt::gap, m.y}, {dx, m.y}, true);
        const juce::Rectangle<float> decoder{dx, box.getY(), rt::mainWidth, rt::boxHeight};
        g.setColour(colour::ground);
        g.fillRect(decoder);
        frame(g, decoder, colour::identity(Product::Decoder), ui::theme::stroke::rule, false);
        ui::text(g, "decoder", ui::labelFont(), colour::text, decoder.withTrimmedLeft(rt::padX));
        addClickArea(decoder, [this] {
            if (showDecoder) showDecoder();
        });
    }

    //  the returns, under the tracks, after a heading
    float y = top + static_cast<float>(layout_.rows) * rt::row + rt::returnsGap;
    y = ui::drawGroupHeading(g, rt::left, y, w - 2.0f * rt::left, "returns");
    for (const Node& r : tree_.nodes) {
        if (r.kind != Node::Kind::Return) continue;
        const bool field = std::find(shape_.returns.begin(), shape_.returns.end(), r.id) != shape_.returns.end();
        const float cy = y + rt::boxHeight / 2.0f;
        const juce::Rectangle<float> box{rt::left, y, rt::boxWidth, rt::boxHeight};
        rows_.push_back({{0.0f, y, w, rt::row}, r.id});
        frame(g, box, field ? colour::ruleStrong : colour::rule, ui::theme::stroke::rule, !field);
        if (hover_ == r.id) hoverRing(g, box);
        ui::text(g, juce::String(r.name), ui::labelFont(), colour::text, box.withTrimmedLeft(rt::padX));
        ui::text(g, field ? "return" : "live", ui::smallFont(), colour::label, box.withTrimmedRight(rt::padX),
                 juce::Justification::centredRight);
        const juce::Rectangle<float> sw{box.getRight() + rt::gap * 2.0f, cy - rt::control / 2.0f,
                                        ui::framedButtonWidth("bambi") * 2.0f, rt::control};
        const auto cells = ui::drawSegments(g, sw, {"live", "bambi"}, field ? 1 : 0, true);
        const auto id = r.id;
        addClickArea(cells[0], [this, id] {
            change([id](live::Shape& s) {
                s.returns.erase(std::remove(s.returns.begin(), s.returns.end(), id), s.returns.end());
            });
        });
        addClickArea(cells[1], [this, id] {
            change([id](live::Shape& s) {
                if (std::find(s.returns.begin(), s.returns.end(), id) == s.returns.end()) s.returns.push_back(id);
            });
        });
        if (field) {
            chain(g, r.id, sw.getRight() + rt::gap * 2.0f, cy);
            if (hover_ == r.id)
                adders(g, r.id, box.getX(), box.getBottom() + rt::control / 2.0f + rt::gap / 2.0f);
            else
                ui::text(g, "hears its sends after the faders", ui::smallFont(), colour::label,
                         {box.getX() + rt::padX, box.getBottom(), w - box.getX(), rt::sendsLine});
        }
        y += rt::row;
    }
    noteContentBottom(y + scroll());
}

float RoutingPage::node(juce::Graphics& g, const Node& n, juce::Point<float> at) {
    const live::Part* part = partOf(n.id);
    const int pair = pairOf(n.id);
    const bool unpaired = part != nullptr && !part->bus && pair < 0;
    const juce::Rectangle<float> box{at.x, at.y - rt::boxHeight / 2.0f, rt::boxWidth, rt::boxHeight};
    rows_.push_back({{box.getX(), box.getY(), rt::boxWidth + controlsWidth(n) + rt::gap, rt::row}, n.id});
    const bool hovered = hover_ == n.id;

    //  outside the field dashed in the rule; a source the encoder's, a bus ink; a source with no pair not chosen
    const juce::Colour c = part == nullptr ? colour::rule
                           : part->bus     ? colour::ruleStrong
                           : unpaired      ? colour::inactive
                                           : colour::identity(Product::Encoder);
    frame(g, box, c, ui::theme::stroke::rule, part == nullptr);
    if (hovered) hoverRing(g, box);
    ui::text(g, juce::String(n.name), ui::labelFont(), unpaired ? colour::inactive : colour::text,
             box.withTrimmedLeft(rt::padX));
    const juce::String kind = part == nullptr ? (n.parent ? "" : "live")
                              : part->bus     ? "bus"
                              : unpaired      ? "no pair"
                                              : "source";
    ui::text(g, kind, ui::smallFont(), colour::label, box.withTrimmedRight(rt::padX),
             juce::Justification::centredRight);

    float x = box.getRight() + rt::gap * 2.0f;
    const auto id = n.id;
    //  the switch, at the top only
    if (!n.parent) {
        const bool inLive = std::find(shape_.inLive.begin(), shape_.inLive.end(), id) != shape_.inLive.end();
        const juce::Rectangle<float> sw{x, at.y - rt::control / 2.0f, ui::framedButtonWidth("bambi") * 2.0f,
                                        rt::control};
        const auto cells = ui::drawSegments(g, sw, {"live", "bambi"}, inLive ? 0 : 1, true);
        addClickArea(cells[0], [this, id] {
            change([id](live::Shape& s) {
                if (std::find(s.inLive.begin(), s.inLive.end(), id) == s.inLive.end()) s.inLive.push_back(id);
            });
        });
        addClickArea(cells[1], [this, id] {
            change([id](live::Shape& s) {
                s.inLive.erase(std::remove(s.inLive.begin(), s.inLive.end(), id), s.inLive.end());
            });
        });
        x = sw.getRight() + rt::gap * 2.0f;
    }
    if (part != nullptr) x = chain(g, id, x, at.y);

    //  under the pointer: what the row offers, on the line under the box, in place of the sends
    if (hovered) {
        const float ly = box.getBottom() + rt::control / 2.0f + rt::gap / 2.0f;
        float lx = box.getX();
        //  a backing in the ground, so the line reads over a connector or a column it reaches into
        const float reach = ui::framedButtonWidth("+ echo") + ui::framedButtonWidth("+ reverb") +
                            ui::framedButtonWidth("encode whole") + 3.0f * rt::gap;
        g.setColour(colour::ground);
        g.fillRect(lx, ly - rt::control / 2.0f - rt::gap / 2.0f, reach, rt::control + rt::gap);
        if (part != nullptr) lx = adders(g, id, lx, ly);
        if (part != nullptr && part->bus) {
            const juce::Rectangle<float> b{lx, ly - rt::control / 2.0f, ui::framedButtonWidth("encode whole"),
                                           rt::control};
            ui::drawFramedButton(g, b, "encode whole", true);
            addClickArea(b, [this, id] {
                change([id](live::Shape& s) {
                    s.split.erase(std::remove(s.split.begin(), s.split.end(), id), s.split.end());
                });
            });
        }
        //  inside a group the field takes whole: the group can become a bus, this track a source of its own
        const live::Part* group = n.parent ? partOf(*n.parent) : nullptr;
        if (part == nullptr && group != nullptr && !group->bus) {
            const auto parent = *n.parent;
            const juce::Rectangle<float> b{lx, ly - rt::control / 2.0f, ui::framedButtonWidth("encode here"),
                                           rt::control};
            ui::drawFramedButton(g, b, "encode here", true);
            addClickArea(b, [this, parent] { change([parent](live::Shape& s) { s.split.push_back(parent); }); });
        }
        return x;
    }

    //  the sends to the returns the field plays, on a second line
    float sx = box.getX() + rt::padX;
    int send = 0;
    for (const Node& r : tree_.nodes) {
        if (r.kind != Node::Kind::Return) continue;
        const int index = send++;
        if (std::find(shape_.returns.begin(), shape_.returns.end(), r.id) == shape_.returns.end()) continue;
        if (index >= static_cast<int>(n.sends.size()) || !n.sends[static_cast<std::size_t>(index)]) continue;
        const juce::String t = juce::String(r.name).substring(0, 1) + " " +
                               dB(*n.sends[static_cast<std::size_t>(index)]) + (part == nullptr ? " lost" : "");
        ui::text(g, t, ui::smallFont(), part == nullptr ? colour::inactive : colour::label,
                 {sx, box.getBottom(), ui::textWidth(ui::smallFont(), t) + 2.0f, rt::sendsLine});
        sx += ui::textWidth(ui::smallFont(), t) + rt::padX;
    }
    return x;
}

float RoutingPage::chain(juce::Graphics& g, live::TrackId owner, float x, float y) {
    const std::vector<live::Insert>& inserts = shape_.insertsOn(owner);
    for (const live::Insert& ins : inserts) {
        const juce::String name = nameOf(ins.effect);
        const float cw = ui::textWidth(ui::labelFont(), name) + 2.0f * rt::padX + rt::control;
        const juce::Rectangle<float> chip{x, y - rt::control / 2.0f, cw, rt::control};
        g.setColour(colour::rule);
        g.drawRect(chip, ui::theme::stroke::rule);
        g.setColour(colourOf(ins.effect));
        g.fillRect(chip.withWidth(ui::theme::stroke::selectionBar));
        ui::text(g, name, ui::labelFont(), colour::text, chip.withTrimmedLeft(rt::padX).withTrimmedRight(rt::control));
        const auto cross = ui::drawCloseCross(g, chip.withTrimmedLeft(cw - rt::control).reduced(rt::gap));
        const auto id = ins.id;
        addClickArea(cross, [this, owner, id, name] {
            confirm_.ask("remove this " + name + "?", "its settings go with it; the host has no undo yet", "remove",
                         [this, owner, id] {
                             change([owner, id](live::Shape& s) {
                                 for (live::Chain& c : s.chains)
                                     if (c.node == owner)
                                         c.inserts.erase(
                                             std::remove_if(c.inserts.begin(), c.inserts.end(),
                                                            [&](const live::Insert& i) { return i.id == id; }),
                                             c.inserts.end());
                             });
                         });
        });
        x = chip.getRight() + rt::gap;
    }
    return x;
}

float RoutingPage::adders(juce::Graphics& g, live::TrackId owner, float x, float y) {
    for (const live::Effect e : {live::Effect::Echo, live::Effect::Reverb}) {
        const juce::String label = "+ " + nameOf(e);
        const juce::Rectangle<float> b{x, y - rt::control / 2.0f, ui::framedButtonWidth(label), rt::control};
        ui::drawFramedButton(g, b, label, true);
        addClickArea(b, [this, owner, e] {
            change([owner, e](live::Shape& s) {
                std::uint64_t next = 1;
                for (const live::Chain& c : s.chains)
                    for (const live::Insert& i : c.inserts) next = std::max(next, i.id + 1);
                auto c = std::find_if(s.chains.begin(), s.chains.end(),
                                      [&](const live::Chain& ch) { return ch.node == owner; });
                if (c == s.chains.end()) c = s.chains.insert(s.chains.end(), live::Chain{owner, {}});
                c->inserts.push_back({next, e});
            });
        });
        x = b.getRight() + rt::gap;
    }
    return x;
}

float RoutingPage::chainWidth(live::TrackId owner) const {
    float w = 0.0f;
    for (const live::Insert& ins : shape_.insertsOn(owner))
        w += ui::textWidth(ui::labelFont(), nameOf(ins.effect)) + 2.0f * rt::padX + rt::control + rt::gap;
    return w;
}

float RoutingPage::controlsWidth(const Node& n) const {
    float w = 0.0f;
    if (!n.parent) w += rt::gap * 2.0f + ui::framedButtonWidth("bambi") * 2.0f;
    if (partOf(n.id) != nullptr) w += rt::gap * 2.0f + chainWidth(n.id);
    return w;
}

}  // namespace bambi::livehost
