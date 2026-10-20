// SPDX-License-Identifier: GPL-3.0-or-later
#include "Routing.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "bambi/ui/Draw.h"
#include "bambi/ui/ProductName.h"
#include "bambi/ui/Widgets.h"

namespace bambi::livehost {

namespace {

namespace colour = ui::theme::colour;
namespace rt = ui::theme::routing;
using live::Node;
using live::Target;

using ui::kSeparator;

juce::String effectName(live::Effect e) { return e == live::Effect::Reverb ? "reverb" : "echo"; }

juce::Colour colourOf(live::Effect e) {
    return colour::identity(e == live::Effect::Reverb ? Product::Reverb : Product::Echo);
}

//  A level as a wire says it: one decimal, a true minus, silence as such.
juce::String dB(double v) {
    if (v <= live::kSilentDb) return juce::String::fromUTF8("\xe2\x88\x92\xe2\x88\x9e");
    return (v > 0.05 ? "+" : "") + ui::signedNumber(v, 1);
}

juce::Font nameFont() { return ui::font(ui::theme::type::body); }

//  The ring round the chosen node, and round what it reaches.
void ring(juce::Graphics& g, juce::Rectangle<float> box) {
    g.setColour(colour::touched);
    g.drawRect(box.expanded(rt::ringGap), ui::theme::stroke::rule);
}

//  A box one line tall, or two when what follows its name does not fit before "send": its chain then goes on the
//  second line.
juce::Rectangle<float> lineBox(float x, float y, float width, bool twoLines) {
    return {x, y + (rt::row - rt::boxHeight) / 2.0f, width, rt::boxHeight + (twoLines ? rt::row : 0.0f)};
}

float bottomOf(juce::Rectangle<float> box) { return box.getBottom() + (rt::row - rt::boxHeight) / 2.0f; }

const juce::String kSendWaits = "click where it goes";
const juce::String kSourceWaits = "click a source";
const juce::String kAttachWaits = "click a part of the field";

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
    if (chosen_ && *chosen_ != live::kMain && partOf(*chosen_) == nullptr &&
        std::none_of(field_.returns.begin(), field_.returns.end(),
                     [&](const live::FieldReturn& r) { return r.node == *chosen_; }))
        chosen_.reset();
    repaint();
}

void RoutingPage::showDecoders(std::vector<live::PlayingDecoder> decoders) {
    playing_ = std::move(decoders);
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

bool RoutingPage::canBeSource(live::TrackId id) const {
    const live::Part* part = partOf(id);
    return part != nullptr && !part->bus && pairOf(id) >= 0;
}

juce::String RoutingPage::nameOf(live::TrackId id) const {
    if (id == live::kMain) return "main";
    for (const Node& n : tree_.nodes)
        if (n.id == id) return juce::String(n.name);
    for (const live::HostInput& in : shape_.inputs)
        if (in.id == id) return juce::String(in.name);
    return "?";
}

juce::String RoutingPage::nameOf(const Target& t) const {
    if (t.kind == Target::Kind::Main) return "main";
    if (t.kind == Target::Kind::Node) return nameOf(t.id);
    for (std::size_t k = 0; k < field_.decoders.size(); ++k)
        if (field_.decoders[k] == t.id)
            return field_.decoders.size() > 1 ? "decoder " + juce::String(static_cast<int>(k) + 1)
                                              : juce::String("decoder");
    return "a decoder";
}

void RoutingPage::change(const std::function<void(live::Shape&)>& edit) {
    live::Shape next = shape_;
    edit(next);
    if (reshape) reshape(std::move(next));
}

bool RoutingPage::keyPressed(const juce::KeyPress& key) {
    if (key != juce::KeyPress::escapeKey) return false;
    if (menu_) {
        menu_.reset();
    } else if (!armed_.escape()) {
        if (!chosen_) return false;
        chosen_.reset();
    }
    repaint();
    return true;
}

// ---- what waits for a click -----------------------------------------------------------------------

void RoutingPage::begin(Waiting w) {
    menu_.reset();
    armed_.begin(w);
    grabKeyboardFocus();
    repaint();
}

bool RoutingPage::waits(Waiting::Kind kind, live::TrackId node, std::uint64_t insert) const {
    const auto& w = armed_.what();
    return w && w->kind == kind && w->node == node && w->insert == insert;
}

bool RoutingPage::takes(live::TrackId node) const {
    const auto& w = armed_.what();
    if (!w) return false;
    switch (w->kind) {
        case Waiting::Kind::Send:
            return w->node != node &&
                   live::canWire(field_, w->node, node == live::kMain ? Target::main() : Target::node(node));
        case Waiting::Kind::Orphan:
            return node == live::kMain || partOf(node) != nullptr ||
                   std::any_of(field_.returns.begin(), field_.returns.end(),
                               [&](const live::FieldReturn& r) { return r.node == node; });
        case Waiting::Kind::Sidechain: return canBeSource(node) && (w->insert != 0 || w->node != node);
    }
    return false;
}

void RoutingPage::take(live::TrackId node) {
    const Waiting w = *armed_.what();
    armed_.end();
    switch (w.kind) {
        case Waiting::Kind::Send: {
            const Target to = node == live::kMain ? Target::main() : Target::node(node);
            change([this, w, to](live::Shape& s) { live::setWire(s, field_, w.node, {to, 0.0}); });
            break;
        }
        case Waiting::Kind::Orphan: attachTo(w.node, node); break;
        case Waiting::Kind::Sidechain: setSidechain(w, node); break;
    }
    repaint();
}

void RoutingPage::setSidechain(Waiting pick, live::TrackId from) {
    change([pick, from](live::Shape& s) {
        if (pick.insert != 0) {
            for (live::Chain& c : s.chains)
                for (live::Insert& i : c.inserts)
                    if (i.id == pick.insert) i.sidechain = from;
            return;
        }
        std::erase_if(s.sidechains, [&](const live::Sidechain& c) { return c.source == pick.node; });
        if (from != 0) s.sidechains.push_back({pick.node, from});
    });
}

void RoutingPage::attachTo(live::TrackId orphan, live::TrackId target) {
    change([orphan, target](live::Shape& s) {
        std::vector<live::Insert> moved;
        for (const live::Chain& c : s.chains)
            if (c.node == orphan) moved = c.inserts;
        std::erase_if(s.chains, [&](const live::Chain& c) { return c.node == orphan; });
        auto to =
            std::find_if(s.chains.begin(), s.chains.end(), [&](const live::Chain& c) { return c.node == target; });
        if (to == s.chains.end()) to = s.chains.insert(s.chains.end(), live::Chain{target, {}});
        to->inserts.insert(to->inserts.end(), moved.begin(), moved.end());
    });
}

// ---- inserts --------------------------------------------------------------------------------------

//  Removing an insert, after asking: its settings go with it, and the host has no undo.
void RoutingPage::removeInsert(live::TrackId owner, std::uint64_t id, juce::String name) {
    confirm_.ask("remove this " + name + "?", "its settings go with it; the host has no undo yet", "remove",
                 [this, owner, id] {
                     change([owner, id](live::Shape& s) {
                         for (live::Chain& c : s.chains)
                             if (c.node == owner)
                                 std::erase_if(c.inserts, [&](const live::Insert& i) { return i.id == id; });
                     });
                 });
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

//  A sidechain that is set, after its chip: what it hears; a click picks another. While it waits for its source,
//  the waiting block. Nothing when none is set and none is picked.
float RoutingPage::sidechainSlot(juce::Graphics& g, live::TrackId heard, Waiting pick, float x, float y) {
    if (waits(Waiting::Kind::Sidechain, pick.node, pick.insert)) {
        const juce::Rectangle<float> box{x, y - rt::control / 2.0f, ui::Armed<Waiting>::width(kSourceWaits),
                                         rt::control};
        armed_.block(*this, g, box, kSourceWaits, [this] { repaint(); });
        return box.getRight() + rt::gap;
    }
    if (heard == 0) return x;
    const bool lost = !canBeSource(heard);
    const juce::String label = "sc " + nameOf(heard) + (lost ? " lost" : "");
    const juce::Rectangle<float> box{x, y - rt::control / 2.0f, ui::framedButtonWidth(label), rt::control};
    ui::drawFramedButton(g, box, label, idle());
    if (idle()) addClickArea(box, [this, pick] { begin(pick); });
    return box.getRight() + rt::gap;
}

float RoutingPage::sidechainWidth(live::TrackId heard, Waiting pick) const {
    if (waits(Waiting::Kind::Sidechain, pick.node, pick.insert))
        return ui::Armed<Waiting>::width(kSourceWaits) + rt::gap;
    if (heard == 0) return 0.0f;
    return ui::framedButtonWidth("sc " + nameOf(heard) + (canBeSource(heard) ? "" : " lost")) + rt::gap;
}

float RoutingPage::chain(juce::Graphics& g, live::TrackId owner, float x, float y) {
    for (const live::Insert& ins : shape_.insertsOn(owner)) {
        const juce::String name = effectName(ins.effect);
        const auto chip =
            ui::drawChip(g, {x, y - rt::control / 2.0f}, rt::control, name, colour::text, colourOf(ins.effect), false);
        const auto id = ins.id;
        chips_[owner].push_back(chip.box);
        if (idle())
            addGestureArea(
                chip.box, [](juce::Point<float>) {},
                [this, owner, id](juce::Point<float> p, bool) {
                    moving_ = Moving{owner, id, p.x};
                    repaint();
                },
                [this] {
                    if (!moving_) return;
                    const Moving m = *moving_;
                    moving_.reset();
                    const std::size_t to = placeAt(m.owner, m.x);
                    change([m, to](live::Shape& s) { live::moveInsert(s, m.owner, m.id, to); });
                },
                [this, id, effect = ins.effect] {
                    if (showInsert) showInsert(id, effect);
                },
                {});
        if (moving_ && moving_->id == id) ui::drawOutOfReach(g, chip.box);
        const float after = dropdown(g, {Menu::Kind::Insert, owner, id, {}}, chip.box);
        x = sidechainSlot(g, ins.sidechain, {Waiting::Kind::Sidechain, owner, id}, after + rt::gap, y);
    }
    if (moving_ && moving_->owner == owner && !chips_[owner].empty()) {
        //  where it would land: a rule before that chip, or after the last
        const auto& boxes = chips_[owner];
        const auto& inserts = shape_.insertsOn(owner);
        std::size_t seen = 0;
        float markX = boxes.back().getRight() + rt::gap / 2.0f;
        const std::size_t to = placeAt(owner, moving_->x);
        for (std::size_t k = 0; k < boxes.size() && k < inserts.size(); ++k) {
            if (inserts[k].id == moving_->id) continue;
            if (seen == to) {
                markX = boxes[k].getX() - rt::gap / 2.0f;
                break;
            }
            ++seen;
        }
        ui::ruleV(g, markX, y - rt::control / 2.0f, y + rt::control / 2.0f, colour::ruleStrong);
    }
    return x;
}

float RoutingPage::chainWidth(live::TrackId owner) const {
    float w = 0.0f;
    for (const live::Insert& ins : shape_.insertsOn(owner))
        w += ui::chipWidth(effectName(ins.effect), rt::control, true, false) + ui::dropdownWidth() + rt::gap +
             sidechainWidth(ins.sidechain, {Waiting::Kind::Sidechain, owner, ins.id});
    return w + ui::framedButtonWidth("+") + rt::gap;
}

std::size_t RoutingPage::placeAt(live::TrackId owner, float x) const {
    const auto c = chips_.find(owner);
    if (c == chips_.end()) return 0;
    //  the place before the first chip whose middle is right of `x`, counted without the chip being moved
    std::size_t place = 0, at = 0;
    const auto& inserts = shape_.insertsOn(owner);
    for (std::size_t k = 0; k < c->second.size() && k < inserts.size(); ++k) {
        if (moving_ && inserts[k].id == moving_->id) continue;
        if (c->second[k].getCentreX() < x) place = at + 1;
        ++at;
    }
    return place;
}

//  "+": the menu of the inserts to add to `owner`.
float RoutingPage::plus(juce::Graphics& g, live::TrackId owner, float x, float y) {
    const juce::Rectangle<float> box{x, y - rt::control / 2.0f, ui::framedButtonWidth("+"), rt::control};
    const bool open = menu_ && menu_->kind == Menu::Kind::Add && menu_->owner == owner;
    ui::drawFramedButton(g, box, "+", idle() || open, open);
    if (idle()) addClickArea(box, [this, owner, box] { openMenu({Menu::Kind::Add, owner, 0, box.getBottomLeft()}); });
    return box.getRight() + rt::gap;
}

void RoutingPage::openMenu(Menu m) {
    menu_ = m;
    repaint();
}

void RoutingPage::menu(juce::Graphics& g) {
    menuItems_.clear();
    if (!menu_) return;
    const Menu m = *menu_;
    addClickArea(getLocalBounds().toFloat(), [this] {
        menu_.reset();
        repaint();
    });
    struct Item {
        ui::MenuItem shown;
        std::function<void()> act;
    };
    std::vector<Item> items;
    juce::String foot;
    if (m.kind == Menu::Kind::Add) {
        items = {{{"echo", "", true}, [this, m] { addInsert(m.owner, live::Effect::Echo); }},
                 {{"reverb", "", true}, [this, m] { addInsert(m.owner, live::Effect::Reverb); }},
                 {{"colour", "later", false}, {}}};
        foot = "an insert, after what " + nameOf(m.owner) + " has";
    } else if (m.kind == Menu::Kind::Encoder) {
        const live::Part* part = partOf(m.owner);
        const live::TrackId heard = part != nullptr ? part->sidechain : 0;
        const auto n =
            std::find_if(tree_.nodes.begin(), tree_.nodes.end(), [&](const Node& x) { return x.id == m.owner; });
        const bool top = n == tree_.nodes.end() || !n->parent;
        items.push_back({{"sidechain", heard != 0 ? nameOf(heard) : juce::String(), true},
                         [this, m] { begin({Waiting::Kind::Sidechain, m.owner, 0}); }});
        if (heard != 0)
            items.push_back(
                {{"no sidechain", "", true}, [this, m] { setSidechain({Waiting::Kind::Sidechain, m.owner, 0}, 0); }});
        if (top && !live::isInput(m.owner))
            items.push_back({{"back to Live", "", true}, [this, m] {
                                 change([this, m](live::Shape& s) { live::removeEncoder(s, tree_, field_, m.owner); });
                             }});
        foot = nameOf(m.owner) + "'s encoder";
    } else {
        live::Insert ins;
        for (const live::Insert& i : shape_.insertsOn(m.owner))
            if (i.id == m.insert) ins = i;
        items.push_back({{"sidechain", ins.sidechain != 0 ? nameOf(ins.sidechain) : juce::String(), true},
                         [this, m] { begin({Waiting::Kind::Sidechain, m.owner, m.insert}); }});
        if (ins.sidechain != 0)
            items.push_back({{"no sidechain", "", true},
                             [this, m] { setSidechain({Waiting::Kind::Sidechain, m.owner, m.insert}, 0); }});
        items.push_back(
            {{"remove", "", true}, [this, m, ins] { removeInsert(m.owner, m.insert, effectName(ins.effect)); }});
        foot = effectName(ins.effect) + " on " + nameOf(m.owner);
    }
    std::vector<ui::MenuItem> shown;
    for (const Item& i : items) shown.push_back(i.shown);
    const auto drawn = ui::drawMenu(g, m.at, rt::menuWidth, shown, foot);
    addClickArea(drawn.box, [] {});
    for (std::size_t i = 0; i < items.size() && i < drawn.items.size(); ++i)
        menuItems_.push_back({items[i].shown.label, drawn.items[i]});
    for (std::size_t i = 0; i < items.size() && i < drawn.items.size(); ++i)
        if (items[i].shown.offered)
            addClickArea(drawn.items[i], [this, act = items[i].act] {
                menu_.reset();
                repaint();
                if (act) act();
            });
}

// ---- wires ----------------------------------------------------------------------------------------

//  "send" at the first line's right end; while it waits, the waiting block there instead.
void RoutingPage::sendButton(juce::Graphics& g, live::TrackId from, juce::Rectangle<float> box) {
    const float cy = box.getY() + rt::boxHeight / 2.0f;
    if (waits(Waiting::Kind::Send, from)) {
        const float w = ui::Armed<Waiting>::width(kSendWaits);
        armed_.block(*this, g, {box.getRight() - rt::gap - w, cy - rt::control / 2.0f, w, rt::control}, kSendWaits,
                     [this] { repaint(); });
        return;
    }
    const float w = ui::framedButtonWidth("send");
    const juce::Rectangle<float> at{box.getRight() - rt::gap - w, cy - rt::control / 2.0f, w, rt::control};
    ui::drawFramedButton(g, at, "send", idle());
    if (idle()) addClickArea(at, [this, from] { begin({Waiting::Kind::Send, from, 0}); });
}

//  The chosen node's wires, under its line, each a chip: dragged sideways for its level, its cross to remove it.
float RoutingPage::wireChips(juce::Graphics& g, live::TrackId from, float x, float y, float width) {
    const auto& wires = field_.wiresOf(from);
    float cx = x;
    if (wires.empty())
        ui::text(g, "wired nowhere: heard by no decoder", ui::smallFont(), colour::label, {x, y, width, rt::row},
                 juce::Justification::centredLeft);
    for (const live::Wire& w : wires) {
        const juce::String label = juce::String::fromUTF8("\xe2\x86\x92 ") + nameOf(w.to) + "  " + dB(w.db) + " dB";
        if (cx + ui::chipWidth(label, rt::control, false, true) > x + width) {
            cx = x;
            y += rt::row;
        }
        const auto chip = ui::drawChip(g, {cx, y + (rt::row - rt::control) / 2.0f}, rt::control, label, colour::text,
                                       w.to.kind == Target::Kind::Decoder
                                           ? std::optional<juce::Colour>{colour::identity(Product::Decoder)}
                                           : std::nullopt,
                                       true);
        const Target to = w.to;
        const double at = w.db;
        auto pressX = std::make_shared<float>(0.0f);
        if (idle()) {
            addGestureArea(
                chip.box.withTrimmedRight(chip.cross.getWidth()),
                [this, pressX](juce::Point<float> p) {
                    *pressX = p.x;
                    dragging_ = shape_;
                },
                [this, pressX, from, to, at](juce::Point<float> p, bool fine) {
                    const double db = std::clamp(
                        std::round((at + (p.x - *pressX) * rt::levelDrag * (fine ? 0.1 : 1.0)) * 10.0) / 10.0,
                        live::kSilentDb, live::kLoudestDb);
                    live::Shape next = dragging_;
                    live::setWire(next, field_, from, {to, db});
                    shape_ = next;
                    field_ = live::layOut(tree_, shape_);
                    if (preview) preview(shape_);
                    repaint();
                },
                [this] {
                    if (reshape) reshape(shape_);
                },
                {},
                [this, from, to] {
                    change([this, from, to](live::Shape& s) { live::setWire(s, field_, from, {to, 0.0}); });
                });
            addClickArea(chip.cross, [this, from, to] {
                change([this, from, to](live::Shape& s) { live::removeWire(s, field_, from, to); });
            });
        }
        cx = chip.box.getRight() + rt::gap;
    }
    return y + rt::row;
}

void RoutingPage::wires(juce::Graphics& g) {
    //  what the chosen node reaches, along its wires
    std::set<live::TrackId> lit;
    std::set<std::uint64_t> litDecoders;
    if (chosen_) {
        std::vector<live::TrackId> todo{*chosen_};
        while (!todo.empty()) {
            const live::TrackId at = todo.back();
            todo.pop_back();
            if (!lit.insert(at).second) continue;
            for (const live::Wire& w : field_.wiresOf(at)) {
                if (w.to.kind == Target::Kind::Decoder)
                    litDecoders.insert(w.to.id);
                else
                    todo.push_back(w.to.kind == Target::Kind::Main ? live::kMain : w.to.id);
            }
        }
    }
    for (const auto& [from, spot] : spots_) {
        for (const live::Wire& w : field_.wiresOf(from)) {
            juce::Point<float> to;
            bool intoRight = false;  // a bus in the same column: entered from its right, round the column's edge
            if (w.to.kind == Target::Kind::Decoder) {
                const auto d = decoderAt_.find(w.to.id);
                if (d == decoderAt_.end()) continue;
                to = {d->second.getX(), d->second.getY() + rt::boxHeight / 2.0f};
            } else {
                const auto s = spots_.find(w.to.kind == Target::Kind::Main ? live::kMain : w.to.id);
                if (s == spots_.end()) continue;
                const auto& box = s->second.box;
                const float cy = box.getY() + rt::boxHeight / 2.0f;
                intoRight = box.getX() < spot.out.x - 1.0f && box.getRight() >= spot.out.x - 1.0f;
                to = intoRight ? juce::Point<float>{box.getRight(), cy + rt::control / 4.0f}
                               : juce::Point<float>{box.getX(), cy};
            }
            const auto from0 = spot.out;
            juce::Path p;
            p.startNewSubPath(from0);
            if (intoRight) {
                const float out = std::max(from0.x, to.x) + rt::wireGutter / 2.0f;
                p.cubicTo({out, from0.y}, {out, to.y}, to);
            } else {
                const float mid = (from0.x + to.x) / 2.0f;
                p.cubicTo({mid, from0.y}, {mid, to.y}, to);
            }
            const bool on = lit.count(from) > 0;
            const juce::Colour ink = on                                   ? colour::ruleStrong
                                     : w.to.kind == Target::Kind::Decoder ? colour::identity(Product::Decoder)
                                                                          : colour::rule;
            g.setColour(ink);
            g.strokePath(p, juce::PathStrokeType(on ? ui::theme::stroke::litWire : ui::theme::stroke::rule));
            if (std::abs(w.db) > 0.05) {
                const juce::String t = dB(w.db);
                const auto mid = p.getPointAlongPath(p.getLength() / 2.0f);
                const float tw = ui::textWidth(ui::smallFont(), t) + 2.0f * rt::labelPad;
                const juce::Rectangle<float> label{mid.x - tw / 2.0f, mid.y - rt::control / 2.0f, tw, rt::control};
                g.setColour(colour::ground);
                g.fillRect(label);
                ui::text(g, t, ui::smallFont(), colour::label, label, juce::Justification::centred);
            }
        }
    }
    for (const live::TrackId id : lit)
        if (id != *chosen_)
            if (const auto s = spots_.find(id); s != spots_.end()) ring(g, s->second.box);
    for (const std::uint64_t d : litDecoders)
        if (const auto b = decoderAt_.find(d); b != decoderAt_.end()) ring(g, b->second);
}

// ---- the page -------------------------------------------------------------------------------------

void RoutingPage::paint(juce::Graphics& g) {
    g.fillAll(colour::ground);
    const auto top = static_cast<float>(ui::theme::metrics::headerHeight);
    setViewport(top, static_cast<float>(getHeight()));
    const float at = top + rt::top - scroll();
    //  where everything goes, drawn into nothing, so the wires can run under the boxes
    spots_.clear();
    decoderAt_.clear();
    chips_.clear();
    {
        juce::Graphics::ScopedSaveState none(g);
        g.reduceClipRegion(juce::Rectangle<int>());
        body(g, at);
    }
    clearRegions();
    chips_.clear();
    {
        const ContentClip clip(*this, g);
        armed_.ground(*this, getLocalBounds().toFloat(), [this] { repaint(); });
        wires(g);
        body(g, at);
    }
    band(g);
    menu(g);
}

void RoutingPage::band(juce::Graphics& g) {
    const auto inner = ui::drawPageBand(g, static_cast<float>(getWidth()));
    const float h = inner.getHeight();
    const int sources = static_cast<int>(field_.sources().size());
    const int paired = static_cast<int>(pairs_.size());
    const auto f = ui::labelFont();
    const juce::String count = juce::String(sources) + (sources == 1 ? " source" : " sources") + kSeparator +
                               juce::String(paired) + " on pairs";
    ui::text(g, count, f, colour::label, {inner.getX(), 0.0f, ui::textWidth(f, count) + 2.0f, h});
    //  every node at the top into the field at once, or every one back to Live
    float x = inner.getRight();
    const auto action = [&](const juce::String& label, std::function<void(live::Shape&)> how) {
        const float aw = ui::framedButtonWidth(label);
        x -= aw;
        const juce::Rectangle<float> at{x, (h - rt::control) / 2.0f, aw, rt::control};
        ui::drawFramedButton(g, at, label, idle());
        if (idle()) addClickArea(at, [this, edit = std::move(how)] { change(edit); });
        x -= rt::gap;
    };
    action("all to Live", [](live::Shape& s) { s.taken.clear(); });
    action("route all", [this](live::Shape& s) { live::takeAll(s, tree_); });
}

juce::Rectangle<float> RoutingPage::frame(juce::Graphics& g, juce::Rectangle<float> box, const juce::String& name,
                                          juce::Colour stripe, bool inField, live::TrackId id) {
    spots_[id] = {box, {box.getRight(), box.getY() + rt::boxHeight / 2.0f}};
    g.setColour(colour::ground);
    g.fillRect(box);
    ui::drawFrame(g, box, inField ? colour::ruleStrong : colour::rule, ui::theme::stroke::rule, !inField);
    g.setColour(stripe);
    g.fillRect(box.withWidth(rt::stripe));
    const juce::Rectangle<float> nameAt{box.getX() + rt::padX, box.getY(), rt::nameWidth, rt::boxHeight};
    ui::text(g, name, nameFont(), colour::text, nameAt, juce::Justification::centredLeft);
    if (chosen_ == id) ring(g, box);
    if (idle() && inField)
        addClickArea(nameAt, [this, id] {
            chosen_ = chosen_ == id ? std::nullopt : std::optional<live::TrackId>{id};
            repaint();
        });
    return nameAt;
}

float RoutingPage::tagWidth() { return ui::textWidth(ui::smallFont(), "in group") + 2.0f * rt::tagPad; }

//  What the node is, in a small outlined tag: in ink while bambi plays it, in the label's colour while Live does.
float RoutingPage::tag(juce::Graphics& g, float x, float cy, const juce::String& label, bool inField) {
    const juce::Rectangle<float> box{x, cy - rt::tagHeight / 2.0f, tagWidth(), rt::tagHeight};
    g.setColour(inField ? colour::ruleStrong : colour::rule);
    g.drawRect(box, ui::theme::stroke::rule);
    ui::text(g, label, ui::smallFont(), inField ? colour::text : colour::label, box, juce::Justification::centred);
    return box.getRight() + rt::gap * 2.0f;
}

float RoutingPage::encoderChip(juce::Graphics& g, live::TrackId id, float x, float y, bool removable) {
    (void)removable;
    const auto chip = ui::drawChip(g, {x, y - rt::control / 2.0f}, rt::control, ui::productName(Product::Encoder),
                                   colour::text, colour::identity(Product::Encoder), false);
    if (idle() && pairOf(id) >= 0)
        addClickArea(chip.box, [this, id] {
            if (showSource) showSource(id);
        });
    return dropdown(g, {Menu::Kind::Encoder, id, 0, {}}, chip.box) + rt::gap;
}

//  The dropdown right after a chip: its menu -- sidechain, taking it away -- while a click on the chip opens it.
float RoutingPage::dropdown(juce::Graphics& g, Menu m, juce::Rectangle<float> chip) {
    const juce::Rectangle<float> box{chip.getRight(), chip.getY(), ui::dropdownWidth(), chip.getHeight()};
    const bool open = menu_ && menu_->kind == m.kind && menu_->owner == m.owner && menu_->insert == m.insert;
    ui::drawDropdown(g, box, open, idle() || open, chip.getHeight());
    m.at = box.getBottomLeft();
    if (idle()) addClickArea(box, [this, m] { openMenu(m); });
    return box.getRight();
}

float RoutingPage::liveNeed(const Node& n, int depth) const {
    const live::Role role = live::roleOf(tree_, field_, n.id);
    float w = rt::indent * static_cast<float>(depth) + rt::padX + rt::nameWidth + rt::gap + tagWidth() + rt::gap * 2.0f;
    if (role == live::Role::Live || role == live::Role::InGroup)
        return w + ui::framedButtonWidth("+ encoder") + rt::padX;
    if (role == live::Role::Bus)
        w += ui::framedButtonWidth("encode whole") + rt::gap * 2.0f;
    else if (const live::Part* part = partOf(n.id))
        w += ui::chipWidth(ui::productName(Product::Encoder), rt::control, true, false) + ui::dropdownWidth() +
             rt::gap + sidechainWidth(part->sidechain, {Waiting::Kind::Sidechain, n.id, 0});
    return w + chainWidth(n.id) + rt::sendRoom;
}

float RoutingPage::liveLine(juce::Graphics& g, const Node& n, int depth, float x, float y, float width) {
    const live::Role role = live::roleOf(tree_, field_, n.id);
    const bool inField = role == live::Role::Source || role == live::Role::Bus;
    const bool unpaired = role == live::Role::Source && pairOf(n.id) < 0;
    const juce::String label = role == live::Role::Live      ? "live"
                               : role == live::Role::Bus     ? "bus"
                               : role == live::Role::InGroup ? "in group"
                               : unpaired                    ? "no pair"
                                                             : "bambi";
    const float indent = rt::indent * static_cast<float>(depth);
    const bool twoLines = inField && liveNeed(n, depth) > width;
    const auto box = lineBox(x + indent, y, width - indent, twoLines);
    const auto nameAt =
        frame(g, box, juce::String(n.name), colour::ofLive(juce::String(n.colour), colour::rule), inField, n.id);
    //  listed under its group as Live files it, but routed past it by hand: where its audio goes, after its name
    if (n.group && n.parent != n.group) {
        const juce::String mark =
            juce::String::fromUTF8("\xe2\x86\x92 ") + (n.path == Node::Path::Main ? "Main" : "elsewhere");
        const float after =
            std::min(nameAt.getRight(), nameAt.getX() + ui::textWidth(nameFont(), juce::String(n.name)) + rt::gap);
        ui::text(g, mark, ui::smallFont(), colour::label,
                 {after, nameAt.getY(), nameAt.getRight() - after, nameAt.getHeight()},
                 juce::Justification::centredLeft);
    }
    const float cy = box.getY() + rt::boxHeight / 2.0f;
    float cx = tag(g, nameAt.getRight() + rt::gap, cy, label, inField && !unpaired);
    const auto id = n.id;
    if (role == live::Role::Live || role == live::Role::InGroup) {
        const juce::Rectangle<float> add{cx, cy - rt::control / 2.0f, ui::framedButtonWidth("+ encoder"), rt::control};
        ui::drawFramedButton(g, add, "+ encoder", idle());
        if (idle())
            addClickArea(add, [this, id] { change([this, id](live::Shape& s) { live::addEncoder(s, tree_, id); }); });
    } else if (role == live::Role::Bus) {
        const juce::Rectangle<float> whole{cx, cy - rt::control / 2.0f, ui::framedButtonWidth("encode whole"),
                                           rt::control};
        ui::drawFramedButton(g, whole, "encode whole", idle());
        if (idle())
            addClickArea(whole, [this, id] {
                change([this, id](live::Shape& s) { live::removeEncoder(s, tree_, field_, id); });
            });
        cx = whole.getRight() + rt::gap * 2.0f;
    } else {
        cx = encoderChip(g, n.id, cx, cy, !n.parent) + rt::gap;
    }
    if (inField) {
        //  the Encoder's sidechain leads the chain: on the second line with it when the first has no room
        const float chainY = twoLines ? cy + rt::row : cy;
        float chainX = twoLines ? nameAt.getRight() + rt::gap : cx;
        if (const live::Part* part = partOf(n.id); part != nullptr && !part->bus)
            chainX = sidechainSlot(g, part->sidechain, {Waiting::Kind::Sidechain, n.id, 0}, chainX, chainY);
        plus(g, n.id, chain(g, n.id, chainX, chainY), chainY);
        sendButton(g, n.id, box);
    }
    if (!waits(Waiting::Kind::Send, n.id)) armed_.over(*this, g, box, takes(n.id), [this, id] { take(id); });
    float bottom = bottomOf(box);
    if (chosen_ == n.id && inField) bottom = wireChips(g, n.id, box.getX() + rt::padX, bottom, box.getWidth());
    return bottom;
}

float RoutingPage::inputLine(juce::Graphics& g, const live::HostInput& in, float x, float y, float width) {
    const live::Part* part = partOf(in.id);
    const float need = rt::padX + rt::nameWidth + rt::gap + tagWidth() + rt::gap * 2.0f +
                       ui::chipWidth(ui::productName(Product::Encoder), rt::control, true, false) +
                       ui::dropdownWidth() + rt::gap +
                       sidechainWidth(part != nullptr ? part->sidechain : 0, {Waiting::Kind::Sidechain, in.id, 0}) +
                       chainWidth(in.id) + rt::sendRoom;
    const bool twoLines = need > width;
    const auto box = lineBox(x, y, width, twoLines);
    const auto nameAt = frame(g, box, juce::String(in.name), colour::label, true, in.id);
    const float cy = box.getY() + rt::boxHeight / 2.0f;
    float cx = tag(g, nameAt.getRight() + rt::gap, cy, in.width == 2 ? "stereo" : "mono", true);
    cx = encoderChip(g, in.id, cx, cy, false) + rt::gap;
    const float chainY = twoLines ? cy + rt::row : cy;
    const float chainX = sidechainSlot(g, part != nullptr ? part->sidechain : 0, {Waiting::Kind::Sidechain, in.id, 0},
                                       twoLines ? nameAt.getRight() + rt::gap : cx, chainY);
    plus(g, in.id, chain(g, in.id, chainX, chainY), chainY);
    sendButton(g, in.id, box);
    const auto id = in.id;
    if (!waits(Waiting::Kind::Send, in.id)) armed_.over(*this, g, box, takes(in.id), [this, id] { take(id); });
    float bottom = bottomOf(box);
    if (chosen_ == in.id) bottom = wireChips(g, in.id, box.getX() + rt::padX, bottom, box.getWidth());
    return bottom;
}

float RoutingPage::returnLine(juce::Graphics& g, const Node& r, float x, float y, float width) {
    const bool inField = std::find(shape_.returns.begin(), shape_.returns.end(), r.id) != shape_.returns.end();
    const float need = rt::padX + rt::nameWidth + rt::gap + tagWidth() + rt::gap * 2.0f +
                       ui::theme::controls::closeGlyph + rt::gap * 2.0f + chainWidth(r.id) + rt::sendRoom;
    const bool twoLines = inField && need > width;
    const auto box = lineBox(x, y, width, twoLines);
    const auto nameAt =
        frame(g, box, juce::String(r.name), colour::ofLive(juce::String(r.colour), colour::rule), inField, r.id);
    const float cy = box.getY() + rt::boxHeight / 2.0f;
    float cx = tag(g, nameAt.getRight() + rt::gap, cy, inField ? "bambi" : "live", inField);
    const auto id = r.id;
    if (!inField) {
        const juce::Rectangle<float> add{cx, cy - rt::control / 2.0f, ui::framedButtonWidth("+ to bambi"), rt::control};
        ui::drawFramedButton(g, add, "+ to bambi", idle());
        if (idle())
            addClickArea(add, [this, id] {
                change([id](live::Shape& s) {
                    if (std::find(s.returns.begin(), s.returns.end(), id) == s.returns.end()) s.returns.push_back(id);
                });
            });
    } else {
        //  the cross after the tag gives the return back to Live
        const float glyph = ui::theme::controls::closeGlyph;
        const juce::Rectangle<float> cross{cx - rt::gap, cy - glyph / 2.0f, glyph, glyph};
        const auto hit = ui::drawCloseCross(g, cross);
        if (idle()) addClickArea(hit, [this, id] { change([id](live::Shape& s) { std::erase(s.returns, id); }); });
        cx = cross.getRight() + rt::gap * 2.0f;
        const float chainY = twoLines ? cy + rt::row : cy;
        plus(g, r.id, chain(g, r.id, twoLines ? nameAt.getRight() + rt::gap : cx, chainY), chainY);
        sendButton(g, r.id, box);
    }
    if (!waits(Waiting::Kind::Send, r.id)) armed_.over(*this, g, box, takes(r.id), [this, id] { take(id); });
    float bottom = bottomOf(box);
    if (chosen_ == r.id && inField) bottom = wireChips(g, r.id, box.getX() + rt::padX, bottom, box.getWidth());
    return bottom;
}

float RoutingPage::mainLine(juce::Graphics& g, float x, float y, float width) {
    const bool twoLines = rt::padX + rt::nameWidth + rt::gap + chainWidth(live::kMain) + rt::sendRoom > width;
    const auto box = lineBox(x, y, width, twoLines);
    const auto nameAt = frame(g, box, "main", colour::ruleStrong, true, live::kMain);
    const float cy = box.getY() + rt::boxHeight / 2.0f;
    const float chainY = twoLines ? cy + rt::row : cy;
    plus(g, live::kMain, chain(g, live::kMain, nameAt.getRight() + rt::gap, chainY), chainY);
    sendButton(g, live::kMain, box);
    if (!waits(Waiting::Kind::Send, live::kMain))
        armed_.over(*this, g, box, takes(live::kMain), [this] { take(live::kMain); });
    float bottom = bottomOf(box);
    if (chosen_ == live::kMain) bottom = wireChips(g, live::kMain, box.getX() + rt::padX, bottom, box.getWidth());
    return bottom;
}

//  A Decoder: its name and cross, what it plays, and what it hears -- all in its box.
float RoutingPage::decoderBox(juce::Graphics& g, std::size_t k, float x, float y, float width) {
    const std::uint64_t id = field_.decoders[k];
    const auto heard = live::heardBy(field_, id);
    //  what it plays, as it reports it; a line only when it has reported
    juce::String plays;
    for (const live::PlayingDecoder& d : playing_)
        if (d.id == id) {
            if (d.report.headphonesOn) plays = "headphones";
            if (d.report.speakersOn)
                plays += (plays.isEmpty() ? juce::String() : kSeparator) +
                         (d.report.layoutName.empty() ? juce::String("speakers") : juce::String(d.report.layoutName));
            if (plays.isEmpty()) plays = "nothing plays";
        }
    const int lines = (plays.isEmpty() ? 0 : 1) + std::max(1, static_cast<int>(heard.size()));
    const juce::Rectangle<float> box{x, y + (rt::row - rt::boxHeight) / 2.0f, width,
                                     rt::boxHeight + static_cast<float>(lines) * rt::hearsLine + rt::gap};
    g.setColour(colour::ground);
    g.fillRect(box);
    ui::drawFrame(g, box, colour::identity(Product::Decoder), ui::theme::stroke::rule, false);
    decoderAt_[id] = box;
    const juce::String name = nameOf(Target::decoder(id));
    const juce::Rectangle<float> nameAt{box.getX() + rt::padX, box.getY(), rt::nameWidth, rt::boxHeight};
    ui::text(g, name, nameFont(), colour::text, nameAt, juce::Justification::centredLeft);
    if (idle())
        addClickArea(nameAt, [this, id] {
            if (showDecoder) showDecoder(id);
        });
    if (field_.decoders.size() > 1) {
        const float glyph = ui::theme::controls::closeGlyph;
        const juce::Rectangle<float> cross{box.getRight() - rt::padX - glyph,
                                           box.getY() + (rt::boxHeight - glyph) / 2.0f, glyph, glyph};
        const auto hit = ui::drawCloseCross(g, cross);
        if (idle())
            addClickArea(hit, [this, id, name] {
                confirm_.ask("remove " + name + "?", "its settings go with it; the host has no undo yet", "remove",
                             [this, id] {
                                 change([this, id](live::Shape& s) {
                                     if (s.decoders.empty()) s.decoders = field_.decoders;
                                     std::erase(s.decoders, id);
                                 });
                             });
            });
    }
    float hy = box.getY() + rt::boxHeight - rt::gap;
    const float inner = width - 2.0f * rt::padX;
    if (plays.isNotEmpty()) {
        ui::text(g, plays, ui::smallFont(), colour::label, {box.getX() + rt::padX, hy, inner, rt::hearsLine},
                 juce::Justification::centredLeft);
        hy += rt::hearsLine;
    }
    if (heard.empty())
        ui::text(g, "hears nothing: silent", ui::smallFont(), colour::label,
                 {box.getX() + rt::padX, hy, inner, rt::hearsLine}, juce::Justification::centredLeft);
    for (const live::Heard& h : heard) {
        const juce::Rectangle<float> line{box.getX() + rt::padX, hy, inner, rt::hearsLine};
        ui::text(g, nameOf(h.node) + (h.twice ? "  heard twice" : ""), ui::smallFont(),
                 h.twice ? colour::text : colour::label, line, juce::Justification::centredLeft);
        ui::text(g, dB(h.db) + " dB", ui::smallFont(), colour::label, line, juce::Justification::centredRight);
        hy += rt::hearsLine;
    }
    const auto& waiting = armed_.what();
    const bool sending = waiting && waiting->kind == Waiting::Kind::Send;
    const Target t = Target::decoder(id);
    armed_.over(*this, g, box, sending && live::canWire(field_, waiting->node, t), [this, t] {
        const Waiting w = *armed_.what();
        armed_.end();
        change([this, w, t](live::Shape& s) { live::setWire(s, field_, w.node, {t, 0.0}); });
    });
    return box.getBottom() + rt::gap * 2.0f;
}

float RoutingPage::orphans(juce::Graphics& g, float y, float width) {
    bool headed = false;
    for (const live::Chain& c : shape_.chains) {
        if (c.node == live::kMain || c.inserts.empty() || live::isInput(c.node)) continue;
        if (std::any_of(tree_.nodes.begin(), tree_.nodes.end(), [&](const Node& n) { return n.id == c.node; }))
            continue;
        if (!headed) {
            y = ui::drawGroupHeading(g, rt::left, y + rt::headingGap, width, "orphaned");
            headed = true;
        }
        const float cy = y + rt::row / 2.0f;
        ui::text(g, "a track Live no longer has", ui::smallFont(), colour::label,
                 {rt::left, y, rt::nameWidth + rt::padX, rt::row});
        float x = rt::left + rt::nameWidth + rt::padX * 2.0f;
        const auto node = c.node;
        if (waits(Waiting::Kind::Orphan, node)) {
            const juce::Rectangle<float> box{x, cy - rt::control / 2.0f, ui::Armed<Waiting>::width(kAttachWaits),
                                             rt::control};
            armed_.block(*this, g, box, kAttachWaits, [this] { repaint(); });
            x = box.getRight() + rt::gap * 2.0f;
        } else {
            const juce::Rectangle<float> at{x, cy - rt::control / 2.0f, ui::framedButtonWidth("attach"), rt::control};
            ui::drawFramedButton(g, at, "attach", idle());
            if (idle()) addClickArea(at, [this, node] { begin({Waiting::Kind::Orphan, node, 0}); });
            x = at.getRight() + rt::gap * 2.0f;
        }
        for (const live::Insert& ins : c.inserts) {
            const auto chip = ui::drawChip(g, {x, cy - rt::control / 2.0f}, rt::control, effectName(ins.effect),
                                           colour::text, colourOf(ins.effect), true);
            const auto owner = c.node;
            const auto insert = ins.id;
            const auto name = effectName(ins.effect);
            if (idle()) addClickArea(chip.cross, [this, owner, insert, name] { removeInsert(owner, insert, name); });
            x = chip.box.getRight() + rt::gap;
        }
        y += rt::row;
    }
    return y;
}

void RoutingPage::body(juce::Graphics& g, float top) {
    const auto w = static_cast<float>(getWidth());
    //  Live's column as wide as its widest line, within bounds; the others after it at even gaps
    const auto lines = live::linesOf(tree_);
    float need = rt::liveLeast;
    for (const live::Line& line : lines) {
        const auto n =
            std::find_if(tree_.nodes.begin(), tree_.nodes.end(), [&](const Node& x) { return x.id == line.node; });
        if (n != tree_.nodes.end()) need = std::max(need, liveNeed(*n, line.depth));
    }
    const float room = w - 2.0f * rt::left - 2.0f * rt::wireGutter - rt::fieldColumn - rt::decoderColumn;
    const float liveWidth = std::clamp(need, rt::liveLeast, std::max(rt::liveLeast, std::min(rt::liveMost, room)));
    const float fieldX = rt::left + liveWidth + rt::wireGutter;
    const float decoderX = fieldX + rt::fieldColumn + rt::wireGutter;

    float y = ui::drawGroupHeading(g, rt::left, top, liveWidth, "live's tracks");
    for (const live::Line& line : lines) {
        const auto n =
            std::find_if(tree_.nodes.begin(), tree_.nodes.end(), [&](const Node& x) { return x.id == line.node; });
        if (n != tree_.nodes.end()) y = liveLine(g, *n, line.depth, rt::left, y, liveWidth);
    }
    if (!shape_.inputs.empty()) {
        y = ui::drawGroupHeading(g, rt::left, y + rt::headingGap, liveWidth, "inputs");
        for (const live::HostInput& in : shape_.inputs) y = inputLine(g, in, rt::left, y, liveWidth);
    }
    y = orphans(g, y, liveWidth);

    //  the returns and Main, one group
    float fy = ui::drawGroupHeading(g, fieldX, top, rt::fieldColumn, "returns and main");
    for (const Node& r : tree_.nodes)
        if (r.kind == Node::Kind::Return) fy = returnLine(g, r, fieldX, fy, rt::fieldColumn);
    fy = mainLine(g, fieldX, fy, rt::fieldColumn);

    //  the Decoders, each with what it plays and hears
    float dy = ui::drawGroupHeading(g, decoderX, top, rt::decoderColumn, "decoders");
    for (std::size_t k = 0; k < field_.decoders.size(); ++k) dy = decoderBox(g, k, decoderX, dy, rt::decoderColumn);
    const juce::Rectangle<float> add{decoderX, dy, ui::framedButtonWidth("+ decoder"), rt::control};
    ui::drawFramedButton(g, add, "+ decoder", idle());
    if (idle())
        addClickArea(add, [this] {
            change([this](live::Shape& s) {
                if (s.decoders.empty()) s.decoders = field_.decoders;
                std::uint64_t next = live::kFirstDecoder;
                for (const std::uint64_t d : s.decoders) next = std::max(next, d + 1);
                s.decoders.push_back(next);
            });
        });
    noteContentBottom(std::max({y, fy, add.getBottom()}) + rt::top + scroll());
}

}  // namespace bambi::livehost
