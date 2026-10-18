// SPDX-License-Identifier: GPL-3.0-or-later
#include "Routing.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "bambi/ui/Draw.h"
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

//  The ring round a box: chosen, or reached from what is chosen; or, dashed, one a waiting send may take.
void ring(juce::Graphics& g, juce::Rectangle<float> box, bool dashed) {
    const auto r = box.expanded(rt::ringGap);
    if (!dashed) {
        g.setColour(colour::touched);
        g.drawRect(r, ui::theme::stroke::rule);
        return;
    }
    juce::Path p;
    p.addRectangle(r);
    g.setColour(colour::learning);
    ui::strokeDashed(g, p, ui::theme::stroke::takesDash, juce::PathStrokeType(ui::theme::stroke::rule));
}

//  A line an armed pick cannot take: under the ground, so it reads as out of reach.
void greyed(juce::Graphics& g, juce::Rectangle<float> box) {
    g.setColour(colour::ground.withAlpha(rt::greyedAlpha));
    g.fillRect(box.expanded(rt::ringGap + 1.0f));
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
    if (chosen_ && *chosen_ != live::kMain && field_.wiresOf(*chosen_).empty() &&
        std::none_of(field_.parts.begin(), field_.parts.end(), [&](const live::Part& p) { return p.node == *chosen_; }))
        chosen_.reset();
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

void RoutingPage::cancel() {
    pick_ = {};
    sending_.reset();
    menuFor_.reset();
    repaint();
}

bool RoutingPage::keyPressed(const juce::KeyPress& key) {
    if (key != juce::KeyPress::escapeKey) return false;
    if (idle() && !menuFor_) {
        if (!chosen_) return false;
        chosen_.reset();
        repaint();
        return true;
    }
    cancel();
    return true;
}

// ---- sidechains and orphans ---------------------------------------------------------------------

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

//  A sidechain's slot: what it hears, or, armed, a block waiting for a source whose click clears it.
float RoutingPage::slot(juce::Graphics& g, float x, float y, live::TrackId heard, bool waiting,
                        std::function<void()> armIt, std::function<void()> clear) {
    const juce::String label = waiting ? juce::String("sc none") : slotLabel(heard);
    const juce::Rectangle<float> box{x, y - rt::control / 2.0f,
                                     ui::textWidth(ui::smallFont(), label) + 2.0f * rt::padX, rt::control};
    if (waiting) {
        ui::drawWaiting(g, box, label, ui::smallFont());
        addClickArea(box, std::move(clear));
        return box.getRight();
    }
    ui::drawFramedButton(g, box, label, idle());
    if (idle()) addClickArea(box, std::move(armIt));
    return box.getRight();
}

void RoutingPage::arm(Pick pick) {
    pick_ = pick;
    sending_.reset();
    menuFor_.reset();
    grabKeyboardFocus();
    repaint();
}

void RoutingPage::pickSource(live::TrackId from) {
    const Pick pick = pick_;
    pick_ = {};
    change([pick, from](live::Shape& s) {
        if (pick.insert != 0) {
            for (live::Chain& c : s.chains)
                for (live::Insert& i : c.inserts)
                    if (i.id == pick.insert) i.sidechain = from;
            return;
        }
        std::erase_if(s.sidechains, [&](const live::Sidechain& c) { return c.source == pick.source; });
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
        std::erase_if(s.chains, [&](const live::Chain& c) { return c.node == orphan; });
        auto to = std::find_if(s.chains.begin(), s.chains.end(), [&](const live::Chain& c) { return c.node == target; });
        if (to == s.chains.end()) to = s.chains.insert(s.chains.end(), live::Chain{target, {}});
        to->inserts.insert(to->inserts.end(), moved.begin(), moved.end());
    });
    repaint();
}

juce::String RoutingPage::waiting() const {
    if (sending_) return "send " + nameOf(*sending_) + kSeparator + "click where it goes" + kSeparator + "esc cancels";
    if (pick_.orphan != 0)
        return "attach the orphaned chain" + kSeparator + "click a part of the field" + kSeparator + "esc cancels";
    juce::String what;
    if (pick_.source != 0) {
        what = nameOf(pick_.source) + "'s encoder";
    } else {
        for (const live::Chain& c : shape_.chains)
            for (const live::Insert& i : c.inserts)
                if (i.id == pick_.insert) what = effectName(i.effect) + " on " + nameOf(c.node);
    }
    return "sidechain for " + what + kSeparator + "click a source" + kSeparator + "esc cancels";
}

// ---- inserts --------------------------------------------------------------------------------------

//  Removing an insert, after asking: its settings go with it, and the host has no undo.
std::function<void()> RoutingPage::removal(live::TrackId owner, std::uint64_t id, juce::String name) {
    return [this, owner, id, name] {
        confirm_.ask("remove this " + name + "?", "its settings go with it; the host has no undo yet", "remove",
                     [this, owner, id] {
                         change([owner, id](live::Shape& s) {
                             for (live::Chain& c : s.chains)
                                 if (c.node == owner)
                                     std::erase_if(c.inserts, [&](const live::Insert& i) { return i.id == id; });
                         });
                     });
    };
}

void RoutingPage::addInsert(live::TrackId owner, live::Effect effect) {
    change([owner, effect](live::Shape& s) {
        std::uint64_t next = 1;
        for (const live::Chain& c : s.chains)
            for (const live::Insert& i : c.inserts) next = std::max(next, i.id + 1);
        auto c = std::find_if(s.chains.begin(), s.chains.end(), [&](const live::Chain& ch) { return ch.node == owner; });
        if (c == s.chains.end()) c = s.chains.insert(s.chains.end(), live::Chain{owner, {}});
        c->inserts.push_back({next, effect, 0});
    });
}

float RoutingPage::chain(juce::Graphics& g, live::TrackId owner, float x, float y) {
    for (const live::Insert& ins : shape_.insertsOn(owner)) {
        const juce::String name = effectName(ins.effect);
        const auto chip =
            ui::drawChip(g, {x, y - rt::control / 2.0f}, rt::control, name, colour::text, colourOf(ins.effect), true);
        const auto id = ins.id;
        const auto effect = ins.effect;
        chips_[owner].push_back(chip.box);
        if (idle())
            addGestureArea(
                chip.box.withTrimmedRight(chip.cross.getWidth()), [](juce::Point<float>) {},
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
                [this, id, effect] {
                    if (showInsert) showInsert(id, effect);
                },
                {});
        if (moving_ && moving_->id == id) {
            g.setColour(colour::ground.withAlpha(rt::greyedAlpha));
            g.fillRect(chip.box);
        }
        if (idle()) addClickArea(chip.cross, removal(owner, id, name));
        x = slot(
                g, chip.box.getRight(), y, ins.sidechain, pick_.insert == id, [this, id] { arm({0, id, 0}); },
                [this] { pickSource(0); }) +
            rt::gap;
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
    ui::drawFramedButton(g, box, "+", idle(), menuFor_ == owner);
    if (idle())
        addClickArea(box, [this, owner, box] {
            menuFor_ = menuFor_ == owner ? std::nullopt : std::optional<live::TrackId>{owner};
            menuAt_ = box.getBottomLeft();
            repaint();
        });
    return box.getRight() + rt::gap;
}

void RoutingPage::menu(juce::Graphics& g) {
    if (!menuFor_) return;
    const live::TrackId owner = *menuFor_;
    addClickArea(getLocalBounds().toFloat(), [this] {
        menuFor_.reset();
        repaint();
    });
    const std::vector<ui::MenuItem> items{{"echo", "", true}, {"reverb", "", true}, {"colour", "later", false}};
    const auto m = ui::drawMenu(g, menuAt_, rt::menuWidth, items, "an insert, after what " + nameOf(owner) + " has");
    addClickArea(m.box, [] {});
    for (std::size_t i = 0; i < 2; ++i)
        addClickArea(m.items[i], [this, owner, i] {
            menuFor_.reset();
            addInsert(owner, i == 0 ? live::Effect::Echo : live::Effect::Reverb);
        });
}

// ---- wires ----------------------------------------------------------------------------------------

bool RoutingPage::targetable(const Target& to) const { return sending_ && live::canWire(field_, *sending_, to); }

//  "send": arms; armed, a click cancels.
void RoutingPage::sendButton(juce::Graphics& g, live::TrackId from, juce::Rectangle<float> box) {
    const juce::String label = "send";
    const juce::Rectangle<float> at{box.getRight() - rt::gap - ui::framedButtonWidth(label),
                                    box.getY() + (rt::boxHeight - rt::control) / 2.0f, ui::framedButtonWidth(label),
                                    rt::control};
    if (sending_ == from) {
        ui::drawWaiting(g, at, label, ui::smallFont());
        addClickArea(at, [this] { cancel(); });
    } else {
        ui::drawFramedButton(g, at, label, idle());
        if (idle())
            addClickArea(at, [this, from] {
                menuFor_.reset();
                sending_ = from;
                grabKeyboardFocus();
                repaint();
            });
    }
}

//  The chosen node's wires, under its line, each a chip: dragged sideways for its level, its cross to remove it.
float RoutingPage::wireChips(juce::Graphics& g, live::TrackId from, float x, float y, float width) {
    const auto& wires = field_.wiresOf(from);
    float cx = x;
    if (wires.empty())
        ui::text(g, "wired nowhere: heard by no decoder", ui::smallFont(), colour::label,
                 {x, y, width, rt::row}, juce::Justification::centredLeft);
    for (const live::Wire& w : wires) {
        const juce::String label = juce::String::fromUTF8("\xe2\x86\x92 ") + nameOf(w.to) + "  " + dB(w.db) + " dB";
        if (cx + ui::chipWidth(label, rt::control, false, true) > x + width) {
            cx = x;
            y += rt::row;
        }
        const auto chip = ui::drawChip(g, {cx, y + (rt::row - rt::control) / 2.0f}, rt::control, label,
                                       colour::text,
                                       w.to.kind == Target::Kind::Decoder
                                           ? std::optional<juce::Colour>{colour::identity(Product::Decoder)}
                                           : std::nullopt,
                                       true);
        const Target to = w.to;
        const double at = w.db;
        auto pressX = std::make_shared<float>(0.0f);
        if (idle()) addGestureArea(
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
            [this, from, to] { change([this, from, to](live::Shape& s) { live::setWire(s, field_, from, {to, 0.0}); }); });
        if (idle())
            addClickArea(chip.cross, [this, from, to] {
                change([this, from, to](live::Shape& s) { live::removeWire(s, field_, from, to); });
            });
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
                if (w.to.kind == Target::Kind::Decoder) litDecoders.insert(w.to.id);
                else todo.push_back(w.to.kind == Target::Kind::Main ? live::kMain : w.to.id);
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
                to = {d->second.getX(), d->second.getCentreY()};
            } else {
                const auto s = spots_.find(w.to.kind == Target::Kind::Main ? live::kMain : w.to.id);
                if (s == spots_.end()) continue;
                const auto& box = s->second.box;
                intoRight = box.getX() < spot.out.x - 1.0f && box.getRight() >= spot.out.x - 1.0f;
                to = intoRight ? juce::Point<float>{box.getRight(), box.getCentreY() + rt::control / 4.0f}
                               : juce::Point<float>{box.getX(), box.getCentreY()};
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
            const juce::Colour ink = on ? colour::ruleStrong
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
    //  what the chosen node reaches, ringed
    for (const live::TrackId id : lit)
        if (id != *chosen_)
            if (const auto s = spots_.find(id); s != spots_.end()) ring(g, s->second.box, false);
    for (const std::uint64_t d : litDecoders)
        if (const auto b = decoderAt_.find(d); b != decoderAt_.end()) ring(g, b->second, false);
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
        //  armed, a click anywhere else cancels: this lies under everything drawn after it
        if (!idle()) addClickArea(getLocalBounds().toFloat(), [this] { cancel(); });
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
    const bool waits = pick_.armed() || sending_;
    const juce::String middle = waits ? waiting()
                                      : juce::String(sources) + (sources == 1 ? " source" : " sources") + kSeparator +
                                            juce::String(paired) + " on pairs";
    ui::text(g, middle, f, waits ? colour::text : colour::label,
             {inner.getX(), 0.0f, ui::textWidth(f, middle) + 2.0f, h});
    //  every node at the top into the field at once, or every one back to Live
    float x = inner.getRight();
    const auto action = [&](const juce::String& label, std::function<void(live::Shape&)> how) {
        const float aw = ui::framedButtonWidth(label);
        x -= aw;
        const juce::Rectangle<float> at{x, (h - rt::control) / 2.0f, aw, rt::control};
        ui::drawFramedButton(g, at, label, !waits);
        if (!waits) addClickArea(at, [this, edit = std::move(how)] { change(edit); });
        x -= rt::gap;
    };
    action("all to Live", [](live::Shape& s) { s.taken.clear(); });
    action("route all", [this](live::Shape& s) { live::takeAll(s, tree_); });
}

bool RoutingPage::takes(live::TrackId node) const {
    if (sending_) return *sending_ != node && targetable(node == live::kMain ? Target::main() : Target::node(node));
    if (pick_.orphan != 0)
        return node == live::kMain || partOf(node) != nullptr ||
               std::any_of(field_.returns.begin(), field_.returns.end(),
                           [&](const live::FieldReturn& r) { return r.node == node; });
    if (pick_.armed()) return canBeSource(node) && pick_.source != node;
    return false;
}

void RoutingPage::take(live::TrackId node) {
    if (sending_) {
        const live::TrackId from = *sending_;
        const Target to = node == live::kMain ? Target::main() : Target::node(node);
        sending_.reset();
        change([this, from, to](live::Shape& s) { live::setWire(s, field_, from, {to, 0.0}); });
    } else if (pick_.orphan != 0) {
        attachTo(node);
    } else {
        pickSource(node);
    }
}

void RoutingPage::armedOver(juce::Graphics& g, juce::Rectangle<float> box, live::TrackId node) {
    if (idle() || sending_ == node || pick_.source == node) return;
    if (!takes(node)) {
        greyed(g, box);
        return;
    }
    ring(g, box, true);
    addClickArea(box, [this, node] { take(node); });
}

float RoutingPage::slotWidth(live::TrackId heard, bool waiting) const {
    return ui::framedButtonWidth(waiting ? juce::String("sc none") : slotLabel(heard));
}

float RoutingPage::chainWidth(live::TrackId owner) const {
    float w = 0.0f;
    for (const live::Insert& ins : shape_.insertsOn(owner))
        w += ui::chipWidth(effectName(ins.effect), rt::control, true, true) +
             slotWidth(ins.sidechain, pick_.insert == ins.id) + rt::gap;
    return w + ui::framedButtonWidth("+") + rt::gap;
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
    ui::text(g, name, ui::labelFont(), colour::text, nameAt, juce::Justification::centredLeft);
    if (chosen_ == id) ring(g, box, false);
    //  the name chooses the node
    if (idle() && inField)
        addClickArea(nameAt, [this, id] {
            chosen_ = chosen_ == id ? std::nullopt : std::optional<live::TrackId>{id};
            menuFor_.reset();
            repaint();
        });
    return nameAt;
}

//  A box one line tall, or two when what follows its name does not fit before "send": its chain then goes on the
//  second line.
static juce::Rectangle<float> lineBox(float x, float y, float width, bool twoLines) {
    return {x, y + (rt::row - rt::boxHeight) / 2.0f, width, rt::boxHeight + (twoLines ? rt::row : 0.0f)};
}

float RoutingPage::encoderChip(juce::Graphics& g, live::TrackId id, float x, float y, bool removable) {
    const auto chip = ui::drawChip(g, {x, y - rt::control / 2.0f}, rt::control, "encoder", colour::text,
                                   colour::identity(Product::Encoder), removable);
    if (idle() && pairOf(id) >= 0)
        addClickArea(chip.box, [this, id] {
            if (showSource) showSource(id);
        });
    if (removable && idle())
        addClickArea(chip.cross, [this, id] {
            change([this, id](live::Shape& s) { live::removeEncoder(s, tree_, field_, id); });
        });
    float right = chip.box.getRight();
    if (const live::Part* part = partOf(id); part != nullptr && !part->bus)
        right = slot(
            g, right, y, part->sidechain, pick_.source == id, [this, id] { arm({id, 0, 0}); },
            [this] { pickSource(0); });
    return right + rt::gap;
}

float RoutingPage::liveLine(juce::Graphics& g, const Node& n, int depth, float x, float y, float width) {
    const live::Role role = live::roleOf(tree_, field_, n.id);
    const bool inField = role == live::Role::Source || role == live::Role::Bus;
    const bool unpaired = role == live::Role::Source && pairOf(n.id) < 0;
    const juce::String mark = role == live::Role::Live      ? "live"
                              : role == live::Role::Bus     ? "bus"
                              : role == live::Role::InGroup ? "in group"
                              : unpaired                    ? "no pair"
                                                            : "bambi";
    const float indent = rt::indent * static_cast<float>(depth);
    const float markWidth = ui::textWidth(ui::smallFont(), "in group") + rt::padX;
    //  what follows the name on the first line, and the chain after it, against the room before "send"
    float first = markWidth;
    if (role == live::Role::Live || role == live::Role::InGroup) first += ui::framedButtonWidth("+ encoder");
    else if (role == live::Role::Bus) first += ui::framedButtonWidth("encode whole") + rt::gap * 2.0f;
    else if (const live::Part* part = partOf(n.id))
        first += ui::chipWidth("encoder", rt::control, true, !n.parent) +
                 slotWidth(part->sidechain, pick_.source == n.id) + rt::gap * 2.0f;
    const float room = width - indent - rt::padX - rt::nameWidth - rt::gap - rt::sendRoom;
    const bool twoLines = inField && first + chainWidth(n.id) > room;
    const auto box = lineBox(x + indent, y, width - indent, twoLines);
    const auto nameAt =
        frame(g, box, juce::String(n.name), colour::ofLive(juce::String(n.colour), colour::rule), inField, n.id);
    const float cy = box.getY() + rt::boxHeight / 2.0f;
    float cx = nameAt.getRight() + rt::gap;
    ui::text(g, mark, ui::smallFont(), unpaired ? colour::inactive : colour::label,
             {cx, cy - rt::control / 2.0f, markWidth, rt::control}, juce::Justification::centredLeft);
    cx += markWidth;
    const auto id = n.id;
    if (role == live::Role::Live || role == live::Role::InGroup) {
        const juce::Rectangle<float> add{cx, cy - rt::control / 2.0f, ui::framedButtonWidth("+ encoder"), rt::control};
        ui::drawFramedButton(g, add, "+ encoder", idle());
        if (idle()) addClickArea(add, [this, id] { change([this, id](live::Shape& s) { live::addEncoder(s, tree_, id); }); });
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
        const float chainY = twoLines ? cy + rt::row : cy;
        cx = twoLines ? nameAt.getRight() + rt::gap : cx;
        plus(g, n.id, chain(g, n.id, cx, chainY), chainY);
        sendButton(g, n.id, box);
    }
    armedOver(g, box, n.id);
    float bottom = box.getBottom() + (rt::row - rt::boxHeight) / 2.0f;
    if (chosen_ == n.id && inField) bottom = wireChips(g, n.id, box.getX() + rt::padX, bottom, box.getWidth());
    return bottom;
}

float RoutingPage::inputLine(juce::Graphics& g, const live::HostInput& in, float x, float y, float width) {
    const float markWidth = ui::textWidth(ui::smallFont(), "in group") + rt::padX;
    const live::Part* part = partOf(in.id);
    const float first = markWidth + ui::chipWidth("encoder", rt::control, true, false) +
                        slotWidth(part != nullptr ? part->sidechain : 0, pick_.source == in.id) + rt::gap * 2.0f;
    const bool twoLines = first + chainWidth(in.id) > width - rt::padX - rt::nameWidth - rt::gap - rt::sendRoom;
    const auto box = lineBox(x, y, width, twoLines);
    const auto nameAt = frame(g, box, juce::String(in.name), colour::label, true, in.id);
    const float cy = box.getY() + rt::boxHeight / 2.0f;
    float cx = nameAt.getRight() + rt::gap;
    ui::text(g, in.width == 2 ? "stereo" : "mono", ui::smallFont(), colour::label,
             {cx, cy - rt::control / 2.0f, markWidth, rt::control}, juce::Justification::centredLeft);
    cx = encoderChip(g, in.id, cx + markWidth, cy, false) + rt::gap;
    const float chainY = twoLines ? cy + rt::row : cy;
    plus(g, in.id, chain(g, in.id, twoLines ? nameAt.getRight() + rt::gap : cx, chainY), chainY);
    sendButton(g, in.id, box);
    armedOver(g, box, in.id);
    float bottom = box.getBottom() + (rt::row - rt::boxHeight) / 2.0f;
    if (chosen_ == in.id) bottom = wireChips(g, in.id, box.getX() + rt::padX, bottom, box.getWidth());
    return bottom;
}

float RoutingPage::returnLine(juce::Graphics& g, const Node& r, float x, float y, float width) {
    const bool inField = std::find(shape_.returns.begin(), shape_.returns.end(), r.id) != shape_.returns.end();
    const float first = ui::segmentsWidth({"live", "bambi"}) + rt::gap * 2.0f;
    const bool twoLines =
        inField && first + chainWidth(r.id) > width - rt::padX - rt::nameWidth - rt::gap - rt::sendRoom;
    const auto box = lineBox(x, y, width, twoLines);
    const auto nameAt =
        frame(g, box, juce::String(r.name), colour::ofLive(juce::String(r.colour), colour::rule), inField, r.id);
    const float cy = box.getY() + rt::boxHeight / 2.0f;
    const juce::Rectangle<float> sw{nameAt.getRight() + rt::gap, cy - rt::control / 2.0f,
                                    ui::segmentsWidth({"live", "bambi"}), rt::control};
    const auto cells = ui::drawSegments(g, sw, {"live", "bambi"}, inField ? 1 : 0, idle());
    const auto id = r.id;
    if (idle()) {
        addClickArea(cells[0], [this, id] { change([id](live::Shape& s) { std::erase(s.returns, id); }); });
        addClickArea(cells[1], [this, id] {
            change([id](live::Shape& s) {
                if (std::find(s.returns.begin(), s.returns.end(), id) == s.returns.end()) s.returns.push_back(id);
            });
        });
    }
    if (inField) {
        const float chainY = twoLines ? cy + rt::row : cy;
        plus(g, r.id, chain(g, r.id, twoLines ? nameAt.getRight() + rt::gap : sw.getRight() + rt::gap * 2.0f, chainY),
             chainY);
        sendButton(g, r.id, box);
    }
    armedOver(g, box, r.id);
    float bottom = box.getBottom() + (rt::row - rt::boxHeight) / 2.0f;
    if (chosen_ == r.id && inField) bottom = wireChips(g, r.id, box.getX() + rt::padX, bottom, box.getWidth());
    return bottom;
}

float RoutingPage::mainLine(juce::Graphics& g, float x, float y, float width) {
    const bool twoLines = chainWidth(live::kMain) > width - rt::padX - rt::nameWidth - rt::gap - rt::sendRoom;
    const auto box = lineBox(x, y, width, twoLines);
    const auto nameAt = frame(g, box, "main", colour::ruleStrong, true, live::kMain);
    const float cy = box.getY() + rt::boxHeight / 2.0f;
    const float chainY = twoLines ? cy + rt::row : cy;
    plus(g, live::kMain, chain(g, live::kMain, nameAt.getRight() + rt::gap, chainY), chainY);
    sendButton(g, live::kMain, box);
    armedOver(g, box, live::kMain);
    float bottom = box.getBottom() + (rt::row - rt::boxHeight) / 2.0f;
    if (chosen_ == live::kMain) bottom = wireChips(g, live::kMain, box.getX() + rt::padX, bottom, box.getWidth());
    return bottom;
}

float RoutingPage::decoderBox(juce::Graphics& g, std::size_t k, float x, float y, float width) {
    const std::uint64_t id = field_.decoders[k];
    const juce::Rectangle<float> box{x, y + (rt::row - rt::boxHeight) / 2.0f, width, rt::boxHeight};
    g.setColour(colour::ground);
    g.fillRect(box);
    ui::drawFrame(g, box, colour::identity(Product::Decoder), ui::theme::stroke::rule, false);
    decoderAt_[id] = box;
    const juce::String name = nameOf(Target::decoder(id));
    const juce::Rectangle<float> nameAt{box.getX() + rt::padX, box.getY(), rt::nameWidth, box.getHeight()};
    ui::text(g, name, ui::labelFont(), colour::text, nameAt, juce::Justification::centredLeft);
    if (idle())
        addClickArea(nameAt, [this, id] {
            if (showDecoder) showDecoder(id);
        });
    if (field_.decoders.size() > 1 && idle()) {
        const juce::Rectangle<float> cross{box.getRight() - rt::padX - ui::theme::controls::closeGlyph,
                                           box.getCentreY() - ui::theme::controls::closeGlyph / 2.0f,
                                           ui::theme::controls::closeGlyph, ui::theme::controls::closeGlyph};
        addClickArea(ui::drawCloseCross(g, cross), [this, id, name] {
            confirm_.ask("remove " + name + "?", "its settings go with it; the host has no undo yet", "remove",
                         [this, id] {
                             change([this, id](live::Shape& s) {
                                 if (s.decoders.empty()) s.decoders = field_.decoders;
                                 std::erase(s.decoders, id);
                             });
                         });
        });
    }
    const Target t = Target::decoder(id);
    if (sending_ && targetable(t)) {
        ring(g, box, true);
        addClickArea(box, [this, t] {
            const auto from = *sending_;
            sending_.reset();
            change([this, from, t](live::Shape& s) { live::setWire(s, field_, from, {t, 0.0}); });
        });
    } else if (!idle()) {
        greyed(g, box);
    }
    //  what it hears, each at its level; a node heard twice says so
    float hy = box.getBottom() + rt::gap;
    const auto heard = live::heardBy(field_, id);
    if (heard.empty()) {
        ui::text(g, "hears nothing: silent", ui::smallFont(), colour::label,
                 {box.getX() + rt::padX, hy, width, rt::hearsLine}, juce::Justification::centredLeft);
        hy += rt::hearsLine;
    }
    for (const live::Heard& h : heard) {
        const juce::Rectangle<float> line{box.getX() + rt::padX, hy, width - 2.0f * rt::padX, rt::hearsLine};
        ui::text(g, nameOf(h.node) + (h.twice ? "  heard twice" : ""), ui::smallFont(),
                 h.twice ? colour::text : colour::label, line, juce::Justification::centredLeft);
        ui::text(g, dB(h.db) + " dB", ui::smallFont(), colour::label, line, juce::Justification::centredRight);
        hy += rt::hearsLine;
    }
    return hy + rt::gap * 2.0f;
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
        const juce::Rectangle<float> attach{x, cy - rt::control / 2.0f, ui::framedButtonWidth("pick a part"),
                                            rt::control};
        if (pick_.orphan == node) {
            ui::drawWaiting(g, attach, "pick a part", ui::smallFont());
            addClickArea(attach, [this] { cancel(); });
        } else {
            const auto at = attach.withWidth(ui::framedButtonWidth("attach"));
            ui::drawFramedButton(g, at, "attach", idle());
            if (idle()) addClickArea(at, [this, node] { arm({0, 0, node}); });
        }
        x = attach.getRight() + rt::gap * 2.0f;
        for (const live::Insert& ins : c.inserts) {
            const auto chip = ui::drawChip(g, {x, cy - rt::control / 2.0f}, rt::control, effectName(ins.effect),
                                           colour::text, colourOf(ins.effect), true);
            if (idle()) addClickArea(chip.cross, removal(c.node, ins.id, effectName(ins.effect)));
            x = chip.box.getRight() + rt::gap;
        }
        y += rt::row;
    }
    return y;
}

void RoutingPage::body(juce::Graphics& g, float top) {
    const auto w = static_cast<float>(getWidth());
    //  the field's and the Decoders' columns give way to Live's in a narrow window, to a floor
    const float fieldWidth = std::clamp(w * 0.25f, rt::fieldColumn * 0.75f, rt::fieldColumn);
    const float decoderWidth = std::clamp(w * 0.17f, rt::decoderColumn * 0.8f, rt::decoderColumn);
    const float decoderX = w - rt::left - decoderWidth;
    const float fieldX = decoderX - rt::wireGutter - fieldWidth;
    const float liveWidth = std::max(rt::nameWidth * 2.0f, fieldX - rt::wireGutter - rt::left);

    //  Live's tracks and groups, then the host's own inputs
    float y = ui::drawGroupHeading(g, rt::left, top, liveWidth, "live's tracks");
    for (const live::Line& line : live::linesOf(tree_)) {
        const auto n = std::find_if(tree_.nodes.begin(), tree_.nodes.end(), [&](const Node& x) { return x.id == line.node; });
        if (n != tree_.nodes.end()) y = liveLine(g, *n, line.depth, rt::left, y, liveWidth);
    }
    if (!shape_.inputs.empty()) {
        y = ui::drawGroupHeading(g, rt::left, y + rt::headingGap, liveWidth, "inputs");
        for (const live::HostInput& in : shape_.inputs) y = inputLine(g, in, rt::left, y, liveWidth);
    }
    y = orphans(g, y, liveWidth);

    //  the returns and Main
    float fy = ui::drawGroupHeading(g, fieldX, top, fieldWidth, "returns");
    for (const Node& r : tree_.nodes)
        if (r.kind == Node::Kind::Return) fy = returnLine(g, r, fieldX, fy, fieldWidth);
    fy = ui::drawGroupHeading(g, fieldX, fy + rt::headingGap, fieldWidth, "main");
    fy = mainLine(g, fieldX, fy, fieldWidth);

    //  the Decoders, each with what it hears
    float dy = ui::drawGroupHeading(g, decoderX, top, decoderWidth, "decoders");
    for (std::size_t k = 0; k < field_.decoders.size(); ++k) dy = decoderBox(g, k, decoderX, dy, decoderWidth);
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
    dy = add.getBottom();
    noteContentBottom(std::max({y, fy, dy}) + rt::top + scroll());
}

}  // namespace bambi::livehost
