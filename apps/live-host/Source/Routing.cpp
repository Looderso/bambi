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

using ui::kSeparator;

juce::String nameOf(live::Effect e) { return e == live::Effect::Reverb ? "reverb" : "echo"; }

juce::Colour colourOf(live::Effect e) {
    return colour::identity(e == live::Effect::Reverb ? Product::Reverb : Product::Echo);
}

//  The ring round a node under the pointer, outside its frame.
void hoverRing(juce::Graphics& g, juce::Rectangle<float> box) {
    g.setColour(colour::touched);
    g.drawRect(box.expanded(rt::ringGap), ui::theme::stroke::rule);
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
    ui::strokeDashed(g, p, ui::theme::stroke::outsideDash, juce::PathStrokeType(ui::theme::stroke::rule));
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
    setWantsKeyboardFocus(true);
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

bool RoutingPage::canBeSource(live::TrackId id) const {
    const live::Part* part = partOf(id);
    return part != nullptr && !part->bus && pairOf(id) >= 0;
}

juce::String RoutingPage::slotLabel(live::TrackId heard) const {
    if (heard == 0) return juce::String("sc ") + juce::String::fromUTF8("\xe2\x80\x93");
    const auto n = std::find_if(tree_.nodes.begin(), tree_.nodes.end(), [&](const Node& x) { return x.id == heard; });
    if (n == tree_.nodes.end()) return "sc lost";
    return "sc " + juce::String(n->name) + (canBeSource(heard) ? "" : " lost");
}

float RoutingPage::slotWidth(live::TrackId heard, bool waiting) const {
    return ui::textWidth(ui::smallFont(), waiting ? juce::String("sc none") : slotLabel(heard)) + 2.0f * rt::padX;
}

//  A sidechain's slot: what it hears, or, armed, a block waiting for a source whose click clears it.
float RoutingPage::slot(juce::Graphics& g, float x, float y, live::TrackId heard, bool waiting,
                        std::function<void()> armIt, std::function<void()> clear) {
    const juce::Rectangle<float> box{x, y - rt::control / 2.0f, slotWidth(heard, waiting), rt::control};
    if (waiting) {
        ui::drawWaiting(g, box, "sc none", ui::smallFont());
        addClickArea(box, std::move(clear));
        return box.getRight();
    }
    g.setColour(colour::rule);
    g.drawRect(box, ui::theme::stroke::rule);
    const bool lost = heard != 0 && !canBeSource(heard);
    ui::text(g, slotLabel(heard), ui::smallFont(), lost ? colour::inactive : colour::label, box,
             juce::Justification::centred);
    if (!pick_.armed()) addClickArea(box, std::move(armIt));
    return box.getRight();
}

void RoutingPage::arm(Pick pick) {
    pick_ = pick;
    grabKeyboardFocus();
    repaint();
}

void RoutingPage::choose(live::TrackId from) {
    const Pick pick = pick_;
    pick_ = {};
    change([pick, from](live::Shape& s) {
        if (pick.insert != 0) {
            for (live::Chain& c : s.chains)
                for (live::Insert& i : c.inserts)
                    if (i.id == pick.insert) i.sidechain = from;
            return;
        }
        s.sidechains.erase(std::remove_if(s.sidechains.begin(), s.sidechains.end(),
                                          [&](const live::Sidechain& c) { return c.source == pick.source; }),
                           s.sidechains.end());
        if (from != 0) s.sidechains.push_back({pick.source, from});
    });
    repaint();
}

void RoutingPage::attachTo(live::TrackId target) {
    const live::TrackId orphan = pick_.orphan;
    pick_ = {};
    change([orphan, target](live::Shape& s) {
        std::vector<live::Insert> moved;
        for (const live::Chain& c : s.chains)
            if (c.node == orphan) moved = c.inserts;
        s.chains.erase(
            std::remove_if(s.chains.begin(), s.chains.end(), [&](const live::Chain& c) { return c.node == orphan; }),
            s.chains.end());
        auto to =
            std::find_if(s.chains.begin(), s.chains.end(), [&](const live::Chain& c) { return c.node == target; });
        if (to == s.chains.end()) to = s.chains.insert(s.chains.end(), live::Chain{target, {}});
        to->inserts.insert(to->inserts.end(), moved.begin(), moved.end());
    });
    repaint();
}

//  What the band says while a slot waits.
juce::String RoutingPage::picking() const {
    if (pick_.orphan != 0)
        return "attach the orphaned chain" + kSeparator + "click a part of the field" + kSeparator + "esc cancels";
    juce::String what;
    if (pick_.source != 0) {
        const auto n =
            std::find_if(tree_.nodes.begin(), tree_.nodes.end(), [&](const Node& x) { return x.id == pick_.source; });
        what = (n != tree_.nodes.end() ? juce::String(n->name) : juce::String()) + "'s encoder";
    } else {
        for (const live::Chain& c : shape_.chains)
            for (const live::Insert& i : c.inserts)
                if (i.id == pick_.insert) {
                    const auto n = std::find_if(tree_.nodes.begin(), tree_.nodes.end(),
                                                [&](const Node& x) { return x.id == c.node; });
                    what = nameOf(i.effect) + " on " +
                           (c.node == live::kMain    ? juce::String("main")
                            : n != tree_.nodes.end() ? juce::String(n->name)
                                                     : juce::String());
                }
    }
    return "sidechain for " + what + kSeparator + "click a source" + kSeparator + "esc cancels";
}

bool RoutingPage::keyPressed(const juce::KeyPress& key) {
    if (key != juce::KeyPress::escapeKey || !pick_.armed()) return false;
    pick_ = {};
    repaint();
    return true;
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
    const auto inner = ui::drawPageBand(g, static_cast<float>(getWidth()));
    const float h = inner.getHeight();
    const int sources = static_cast<int>(field_.sources().size());
    const int paired = static_cast<int>(pairs_.size());
    const juce::String count =
        juce::String(sources) + (sources == 1 ? " source" : " sources") + kSeparator + juce::String(paired) + " on pairs";
    const juce::String toMain = juce::String("field ") + juce::String::fromUTF8("\xe2\x86\x92") + " main";
    const auto f = ui::labelFont();
    ui::text(g, "live", f, colour::label, {inner.getX(), 0.0f, ui::textWidth(f, "live") + 2.0f, h});
    const juce::String middle = pick_.armed() ? picking() : count;
    ui::text(g, middle, f, pick_.armed() ? colour::text : colour::label,
             {inner.getX() + rt::boxWidth, 0.0f, ui::textWidth(f, middle) + 2.0f, h});
    const float tw = ui::textWidth(f, toMain) + 2.0f;
    ui::text(g, toMain, f, colour::label, {inner.getRight() - tw, 0.0f, tw, h});
}

void RoutingPage::body(juce::Graphics& g, float top) {
    const auto w = static_cast<float>(getWidth());
    //  armed, a click anywhere else cancels: this lies under everything drawn after it
    if (pick_.armed())
        addClickArea(getLocalBounds().toFloat(), [this] {
            pick_ = {};
            repaint();
        });
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
        const auto offers = offersFor(nullptr, live::kMain);
        rows_.push_back(
            {{box.getX(), box.getY(),
              std::max(rt::mainWidth + chainWidth(live::kMain) + rt::gap * 3.0f, offersWidth(offers)), rt::row},
             kHoverMain});
        ui::drawFrame(g, box, colour::ruleStrong, ui::theme::stroke::rule, false);
        if (hover_ == kHoverMain) hoverRing(g, box);
        ui::text(g, "main", ui::labelFont(), colour::text, box.withTrimmedLeft(rt::padX));
        if (pick_.orphan != 0) addClickArea(box, [this] { attachTo(live::kMain); });
        float x = chain(g, live::kMain, box.getRight() + rt::gap * 2.0f, m.y);
        if (hover_ == kHoverMain) offer(g, box.getX(), box.getBottom() + rt::control / 2.0f + rt::gap / 2.0f, offers);
        //  the decoders, each a node of its own after Main's chain, in their plugin's colour, one under the other: a
        //  click shows one, its cross removes it after asking -- never the last -- and "+ decoder" adds a listener
        const float dx = std::min(x + rt::gutterLeast, w - rt::left - rt::decoderWidth);
        const auto& decoders = field_.decoders;
        float dy = box.getY();
        for (std::size_t k = 0; k < decoders.size(); ++k) {
            const std::uint64_t id = decoders[k];
            const juce::Rectangle<float> decoder{dx, dy, rt::decoderWidth, rt::boxHeight};
            connect(g, {x - rt::gap, m.y}, {dx, decoder.getCentreY()}, true);
            g.setColour(colour::ground);
            g.fillRect(decoder);
            ui::drawFrame(g, decoder, colour::identity(Product::Decoder), ui::theme::stroke::rule, false);
            const live::TrackId hover = kHoverDecoder - k;
            rows_.push_back({decoder, hover});
            if (hover_ == hover) hoverRing(g, decoder);
            const juce::String name = decoders.size() > 1 ? "decoder " + juce::String(static_cast<int>(k) + 1)
                                                          : juce::String("decoder");
            ui::text(g, name, ui::labelFont(), colour::text, decoder.withTrimmedLeft(rt::padX));
            addClickArea(decoder, [this, id] {
                if (showDecoder) showDecoder(id);
            });
            if (decoders.size() > 1) {
                const juce::Rectangle<float> cross{decoder.getRight() - rt::padX - ui::theme::controls::closeGlyph,
                                                   decoder.getCentreY() - ui::theme::controls::closeGlyph / 2.0f,
                                                   ui::theme::controls::closeGlyph, ui::theme::controls::closeGlyph};
                addClickArea(ui::drawCloseCross(g, cross), [this, id, name] {
                    confirm_.ask("remove " + name + "?", "its settings go with it; the host has no undo yet", "remove",
                                 [this, id] {
                                     change([this, id](live::Shape& s) {
                                         if (s.decoders.empty()) s.decoders = field_.decoders;
                                         s.decoders.erase(std::remove(s.decoders.begin(), s.decoders.end(), id),
                                                          s.decoders.end());
                                     });
                                 });
                });
            }
            dy += rt::boxHeight + rt::gap;
        }
        const auto add = ui::drawChip(g, {dx, dy}, rt::control, "+ decoder", colour::label);
        addClickArea(add.box, [this] {
            change([this](live::Shape& s) {
                if (s.decoders.empty()) s.decoders = field_.decoders;
                std::uint64_t next = live::kFirstDecoder;
                for (const std::uint64_t d : s.decoders) next = std::max(next, d + 1);
                s.decoders.push_back(next);
            });
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
        ui::drawFrame(g, box, field ? colour::ruleStrong : colour::rule, ui::theme::stroke::rule, !field);
        if (hover_ == r.id) hoverRing(g, box);
        ui::text(g, juce::String(r.name), ui::labelFont(), colour::text, box.withTrimmedLeft(rt::padX));
        if (field && pick_.orphan != 0) {
            const auto rid = r.id;
            addClickArea(box, [this, rid] { attachTo(rid); });
        }
        ui::text(g, field ? "return" : "live", ui::smallFont(), colour::label, box.withTrimmedRight(rt::padX),
                 juce::Justification::centredRight);
        const juce::Rectangle<float> sw{box.getRight() + rt::gap * 2.0f, cy - rt::control / 2.0f,
                                        ui::segmentsWidth({"live", "bambi"}), rt::control};
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
                offer(g, box.getX(), box.getBottom() + rt::control / 2.0f + rt::gap / 2.0f, offersFor(nullptr, r.id));
            else
                ui::text(g, "hears its sends after the faders", ui::smallFont(), colour::label,
                         {box.getX() + rt::padX, box.getBottom(), w - box.getX(), rt::sendsLine});
        }
        y += rt::row;
    }
    //  the chains whose track Live no longer has: kept, never dropped unseen, removable insert by insert
    bool headed = false;
    for (const live::Chain& c : shape_.chains) {
        if (c.node == live::kMain || c.inserts.empty()) continue;
        if (std::any_of(tree_.nodes.begin(), tree_.nodes.end(), [&](const Node& n) { return n.id == c.node; }))
            continue;
        if (!headed) {
            y = ui::drawGroupHeading(g, rt::left, y + rt::returnsGap, w - 2.0f * rt::left, "orphaned");
            headed = true;
        }
        const float cy = y + rt::boxHeight / 2.0f;
        ui::text(g, "a track Live no longer has", ui::smallFont(), colour::label,
                 {rt::left, y, rt::boxWidth, rt::boxHeight});
        float x = rt::left + rt::boxWidth + rt::gap * 2.0f;
        const auto node = c.node;
        const juce::Rectangle<float> attach{x, cy - rt::control / 2.0f, ui::framedButtonWidth("pick a part"),
                                            rt::control};
        if (pick_.orphan == node) {
            ui::drawWaiting(g, attach, "pick a part", ui::font(ui::theme::type::body));
            addClickArea(attach, [this] {
                pick_ = {};
                repaint();
            });
        } else {
            ui::drawFramedButton(g, attach.withWidth(ui::framedButtonWidth("attach")), "attach", !pick_.armed());
            if (!pick_.armed())
                addClickArea(attach.withWidth(ui::framedButtonWidth("attach")), [this, node] { arm({0, 0, node}); });
        }
        x = attach.getRight() + rt::gap * 2.0f;
        for (const live::Insert& ins : c.inserts) {
            const auto chip = ui::drawChip(g, {x, cy - rt::control / 2.0f}, rt::control, nameOf(ins.effect),
                                           colour::text, colourOf(ins.effect), true);
            addClickArea(chip.cross, removal(c.node, ins.id, nameOf(ins.effect)));
            x = chip.box.getRight() + rt::gap;
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
    const auto offers = offersFor(&n, n.id);
    rows_.push_back(
        {{box.getX(), box.getY(), std::max(rt::boxWidth + controlsWidth(n) + rt::gap, offersWidth(offers)), rt::row},
         n.id});
    const bool hovered = hover_ == n.id;

    //  outside the field dashed in the rule; a source the encoder's, a bus ink; a source with no pair not chosen.
    //  While a sidechain slot waits, all that cannot be its source is greyed.
    const bool pickable =
        pick_.orphan != 0 ? part != nullptr : pick_.armed() && canBeSource(n.id) && pick_.source != n.id;
    const bool greyed = unpaired || (pick_.armed() && !pickable);
    const juce::Colour c = greyed && pick_.armed() ? colour::inactive
                           : part == nullptr       ? colour::rule
                           : part->bus             ? colour::ruleStrong
                           : unpaired              ? colour::inactive
                                                   : colour::identity(Product::Encoder);
    ui::drawFrame(g, box, c, ui::theme::stroke::rule, part == nullptr);
    if (hovered && (!pick_.armed() || pickable)) hoverRing(g, box);
    ui::text(g, juce::String(n.name), ui::labelFont(), greyed ? colour::inactive : colour::text,
             box.withTrimmedLeft(rt::padX));
    const auto id = n.id;
    if (pickable)
        addClickArea(box, [this, id] {
            if (pick_.orphan != 0)
                attachTo(id);
            else
                choose(id);
        });
    else if (!pick_.armed() && part != nullptr && !part->bus && pair >= 0)
        addClickArea(box, [this, id] {
            if (showSource) showSource(id);
        });
    const juce::String kind = part == nullptr ? (n.parent ? "" : "live")
                              : part->bus     ? "bus"
                              : unpaired      ? "no pair"
                                              : "source";
    ui::text(g, kind, ui::smallFont(), colour::label, box.withTrimmedRight(rt::padX),
             juce::Justification::centredRight);

    float x = box.getRight() + rt::gap * 2.0f;
    //  a source's Encoder's sidechain
    if (part != nullptr && !part->bus) {
        const bool waiting = pick_.source == id;
        x = slot(
                g, x, at.y, part->sidechain, waiting, [this, id] { arm({id, 0}); }, [this] { choose(0); }) +
            rt::gap * 2.0f;
    }
    //  the switch, at the top only
    if (!n.parent) {
        const bool inLive = std::find(shape_.inLive.begin(), shape_.inLive.end(), id) != shape_.inLive.end();
        const juce::Rectangle<float> sw{x, at.y - rt::control / 2.0f, ui::segmentsWidth({"live", "bambi"}),
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
    if (hovered && !offers.empty() && !pick_.armed()) {
        offer(g, box.getX(), box.getBottom() + rt::control / 2.0f + rt::gap / 2.0f, offers);
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
    for (const live::Insert& ins : shape_.insertsOn(owner)) {
        const juce::String name = nameOf(ins.effect);
        const auto chip =
            ui::drawChip(g, {x, y - rt::control / 2.0f}, rt::control, name, colour::text, colourOf(ins.effect), true);
        const auto id = ins.id;
        const auto effect = ins.effect;
        if (!pick_.armed())
            addClickArea(chip.box, [this, id, effect] {
                if (showInsert) showInsert(id, effect);
            });
        addClickArea(chip.cross, removal(owner, id, name));
        x = slot(
                g, chip.box.getRight(), y, ins.sidechain, pick_.insert == id, [this, id] { arm({0, id}); },
                [this] { choose(0); }) +
            rt::gap;
    }
    return x;
}

//  Removing an insert, after asking: its settings go with it, and the host has no undo.
std::function<void()> RoutingPage::removal(live::TrackId owner, std::uint64_t id, juce::String name) {
    return [this, owner, id, name] {
        confirm_.ask("remove this " + name + "?", "its settings go with it; the host has no undo yet", "remove",
                     [this, owner, id] {
                         change([owner, id](live::Shape& s) {
                             for (live::Chain& c : s.chains)
                                 if (c.node == owner)
                                     c.inserts.erase(std::remove_if(c.inserts.begin(), c.inserts.end(),
                                                                    [&](const live::Insert& i) { return i.id == id; }),
                                                     c.inserts.end());
                         });
                     });
    };
}

void RoutingPage::addInsert(live::TrackId owner, live::Effect effect) {
    change([owner, effect](live::Shape& s) {
        std::uint64_t next = 1;
        for (const live::Chain& c : s.chains)
            for (const live::Insert& i : c.inserts) next = std::max(next, i.id + 1);
        auto c =
            std::find_if(s.chains.begin(), s.chains.end(), [&](const live::Chain& ch) { return ch.node == owner; });
        if (c == s.chains.end()) c = s.chains.insert(s.chains.end(), live::Chain{owner, {}});
        c->inserts.push_back({next, effect, 0});
    });
}

//  What a row offers under the pointer: inserts for whatever the field plays -- a part, Main, a return in it -- and
//  for a bus "encode whole"; for a track inside a group the field takes whole, "encode here".
std::vector<RoutingPage::Offer> RoutingPage::offersFor(const Node* n, live::TrackId owner) {
    std::vector<Offer> out;
    const live::Part* part = n != nullptr ? partOf(n->id) : nullptr;
    if (n == nullptr || part != nullptr)
        for (const live::Effect e : {live::Effect::Echo, live::Effect::Reverb})
            out.push_back({"+ " + nameOf(e), [this, owner, e] { addInsert(owner, e); }});
    if (part != nullptr && part->bus)
        out.push_back({"encode whole", [this, owner] {
                           change([owner](live::Shape& s) {
                               s.split.erase(std::remove(s.split.begin(), s.split.end(), owner), s.split.end());
                           });
                       }});
    const live::Part* group = n != nullptr && n->parent ? partOf(*n->parent) : nullptr;
    if (n != nullptr && part == nullptr && group != nullptr && !group->bus) {
        const auto parent = *n->parent;
        out.push_back(
            {"encode here", [this, parent] { change([parent](live::Shape& s) { s.split.push_back(parent); }); }});
    }
    return out;
}

float RoutingPage::offersWidth(const std::vector<Offer>& offers) {
    float w = 0.0f;
    for (const Offer& o : offers) w += ui::framedButtonWidth(o.label) + rt::gap;
    return w;
}

//  The buttons, on a backing in the ground as wide as they are, so they read over a connector or a column they reach.
void RoutingPage::offer(juce::Graphics& g, float x, float y, const std::vector<Offer>& offers) {
    if (offers.empty()) return;
    g.setColour(colour::ground);
    g.fillRect(x, y - rt::control / 2.0f - rt::gap / 2.0f, offersWidth(offers), rt::control + rt::gap);
    for (const Offer& o : offers) {
        const juce::Rectangle<float> b{x, y - rt::control / 2.0f, ui::framedButtonWidth(o.label), rt::control};
        ui::drawFramedButton(g, b, o.label, true);
        addClickArea(b, o.act);
        x = b.getRight() + rt::gap;
    }
}

float RoutingPage::chainWidth(live::TrackId owner) const {
    float w = 0.0f;
    for (const live::Insert& ins : shape_.insertsOn(owner))
        w += ui::chipWidth(nameOf(ins.effect), rt::control, true, true) +
             slotWidth(ins.sidechain, pick_.insert == ins.id) + rt::gap;
    return w;
}

float RoutingPage::controlsWidth(const Node& n) const {
    float w = 0.0f;
    if (!n.parent) w += rt::gap * 2.0f + ui::segmentsWidth({"live", "bambi"});
    if (const live::Part* part = partOf(n.id); part != nullptr && !part->bus)
        w += rt::gap * 2.0f + slotWidth(part->sidechain, pick_.source == n.id);
    if (partOf(n.id) != nullptr) w += rt::gap * 2.0f + chainWidth(n.id);
    return w;
}

}  // namespace bambi::livehost
