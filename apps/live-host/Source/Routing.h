// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "bambi/live/field.hpp"
#include "bambi/live/io.hpp"
#include "bambi/live/page.hpp"
#include "bambi/ui/Armed.h"
#include "bambi/ui/ConfirmBox.h"
#include "bambi/ui/HitArea.h"

/*  The live host's routing page: where everything the host plays sends its audio. Three columns, wires between
 *  them: Live's tracks and groups, as Live lists them, then the host's own inputs; Live's returns and Main; the
 *  Decoders, each with what it plays and what it hears. What it changes is the shape; the host lays the field out
 *  again and routes Live from it.
 *
 *    - a node's line: Live's colour, its name, a tag -- live, bambi, bus, in group -- then "+ encoder", which takes
 *      it to bambi, or its Encoder as a chip; its inserts as chips, dragged along the chain to move one; "+", a menu
 *      of the inserts to add; "send". A chip's click opens its plugin; the dropdown after it, its menu: its
 *      sidechain, taking it away -- an Encoder away gives the node back to Live, an insert goes after asking. A sidechain set shows after its chip,
 *      and a click on it picks another. A bus offers "encode whole". Every node starts in Live; "route all", in the
 *      band, takes each at the top, and "all to Live" gives every one back;
 *    - a return: the same tag, "+ to bambi" while it plays in Live, and a cross after the tag to give it back;
 *      Main: its chain, "+" and "send";
 *    - a Decoder: its name, what it plays, a cross that removes it after asking -- never the last -- and what it
 *      hears, each at its level, a node heard twice marked; "+ decoder" adds a listener. A click on its name shows
 *      it;
 *    - "send", a sidechain's pick and an orphaned chain's "attach" wait for the next click, as every page's picks
 *      do: what may answer is ringed, the rest out of reach;
 *    - the wires, drawn from each node's send to where it plays, a level that is not 0 dB on them; a click on a
 *      node's name chooses it, lighting everything it reaches, and its wires open under its line as chips: drag one
 *      sideways for its level, shift finer, double click for 0 dB; its cross removes the wire.
 *
 *  A chain whose track Live no longer has is listed under "orphaned", its inserts removable one by one: kept, never
 *  dropped unseen; "attach" moves the whole chain to a part of the field, Main or a return it plays, after that
 *  part's own inserts. A chain on a node the field no longer takes is kept, for when it is taken again.
 *
 *  Live's column is as wide as its widest line, within bounds; the others follow at even gaps. The band stays while
 *  the rest scrolls. Clicks on release; a level and an insert are dragged. Message thread.
 */
namespace bambi::livehost {

class RoutingPage final : public ui::HitArea {
public:
    RoutingPage();

    /// What Live's set is now, and what the field makes of it.
    void show(const live::Tree& tree, const live::Shape& shape, const live::Field& field,
              const std::vector<live::TrackPair>& pairs);
    /// What each Decoder plays now, as it reports it.
    void showDecoders(std::vector<live::PlayingDecoder> decoders);

    std::function<void(live::Shape)> reshape;                     ///< the user changed the shape
    std::function<void(const live::Shape&)> preview;              ///< a level being dragged: to hear, not to keep
    std::function<void(std::uint64_t)> showDecoder;               ///< a decoder's name was clicked, by its id
    std::function<void(live::TrackId)> showSource;                ///< a source's Encoder chip: its Encoder
    std::function<void(std::uint64_t, live::Effect)> showInsert;  ///< an insert's chip: its plugin

    /// Where a node's line -- a part's, a return's, Main's (kMain) -- and a Decoder's box were last drawn; none
    /// when they were not. For checks.
    std::optional<juce::Rectangle<float>> drawnAt(live::TrackId node) const {
        const auto s = spots_.find(node);
        return s == spots_.end() ? std::nullopt : std::optional<juce::Rectangle<float>>{s->second.box};
    }
    std::optional<juce::Rectangle<float>> drawnDecoderAt(std::uint64_t id) const {
        const auto d = decoderAt_.find(id);
        return d == decoderAt_.end() ? std::nullopt : std::optional<juce::Rectangle<float>>{d->second};
    }
    /// The open menu's items, as last drawn: what each says and where; none while no menu is open. For checks.
    const std::vector<std::pair<juce::String, juce::Rectangle<float>>>& drawnMenu() const { return menuItems_; }
    /// The node whose wires are open under its line; none when none is chosen. For checks.
    void choose(std::optional<live::TrackId> node) {
        chosen_ = node;
        repaint();
    }

    void paint(juce::Graphics& g) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;

private:
    //  Where a node that wires is, as last drawn: its box, and where its wires leave.
    struct Spot {
        juce::Rectangle<float> box;
        juce::Point<float> out;
    };
    //  What waits for the next click: a node's send, for where its wire goes; an Encoder's or an insert's
    //  sidechain, for its source; an orphaned chain, for the part that takes it.
    struct Waiting {
        enum class Kind { Send, Sidechain, Orphan };
        Kind kind{Kind::Send};
        live::TrackId node{0};    ///< the sending node, the source whose Encoder it is, the chain's former track
        std::uint64_t insert{0};  ///< a sidechain's insert; 0 an Encoder's
    };
    //  A menu dropped from a control: "+"'s inserts, an Encoder's, an insert's.
    struct Menu {
        enum class Kind { Add, Encoder, Insert };
        Kind kind{Kind::Add};
        live::TrackId owner{0};
        std::uint64_t insert{0};
        juce::Point<float> at;
    };
    //  An insert chip being dragged along its chain: whose, which, and where the pointer is.
    struct Moving {
        live::TrackId owner{0};
        std::uint64_t id{0};
        float x{0.0f};
    };

    void band(juce::Graphics& g);
    void body(juce::Graphics& g, float top);
    float liveLine(juce::Graphics& g, const live::Node& n, int depth, float x, float y, float width);
    float inputLine(juce::Graphics& g, const live::HostInput& in, float x, float y, float width);
    float returnLine(juce::Graphics& g, const live::Node& r, float x, float y, float width);
    float mainLine(juce::Graphics& g, float x, float y, float width);
    float decoderBox(juce::Graphics& g, std::size_t k, float x, float y, float width);
    float orphans(juce::Graphics& g, float y, float width);
    //  A line's frame, its stripe and name; where its controls start.
    juce::Rectangle<float> frame(juce::Graphics& g, juce::Rectangle<float> box, const juce::String& name,
                                 juce::Colour stripe, bool inField, live::TrackId id);
    float tag(juce::Graphics& g, float x, float cy, const juce::String& label, bool inField);
    float encoderChip(juce::Graphics& g, live::TrackId id, float x, float y, bool removable);
    float sidechainSlot(juce::Graphics& g, live::TrackId heard, Waiting pick, float x, float y);
    float chain(juce::Graphics& g, live::TrackId owner, float x, float y);
    float plus(juce::Graphics& g, live::TrackId owner, float x, float y);
    void sendButton(juce::Graphics& g, live::TrackId from, juce::Rectangle<float> box);
    float wireChips(juce::Graphics& g, live::TrackId from, float x, float y, float width);
    void wires(juce::Graphics& g);
    void menu(juce::Graphics& g);
    void openMenu(Menu m);
    float dropdown(juce::Graphics& g, Menu m, juce::Rectangle<float> chip);

    //  What the lines need, measured as they are drawn: the column widths follow.
    float liveNeed(const live::Node& n, int depth) const;
    float chainWidth(live::TrackId owner) const;
    float sidechainWidth(live::TrackId heard, Waiting pick) const;
    static float tagWidth();

    bool idle() const { return !armed_.on() && !menu_; }  ///< nothing waits for a click
    bool takes(live::TrackId node) const;                 ///< whether `node`'s line answers what waits
    void take(live::TrackId node);
    void begin(Waiting w);
    bool waits(Waiting::Kind kind, live::TrackId node, std::uint64_t insert = 0) const;
    std::size_t placeAt(live::TrackId owner, float x) const;  ///< the place in `owner`'s chain at `x`

    juce::String nameOf(live::TrackId id) const;
    juce::String nameOf(const live::Target& t) const;
    const live::Part* partOf(live::TrackId id) const;
    int pairOf(live::TrackId id) const;
    bool canBeSource(live::TrackId id) const;
    void change(const std::function<void(live::Shape&)>& edit);
    void addInsert(live::TrackId owner, live::Effect effect);
    void setSidechain(Waiting pick, live::TrackId from);
    void attachTo(live::TrackId orphan, live::TrackId target);
    void removeInsert(live::TrackId owner, std::uint64_t id, juce::String name);

    live::Tree tree_;
    live::Shape shape_;
    live::Field field_;
    std::vector<live::TrackPair> pairs_;
    std::vector<live::PlayingDecoder> playing_;
    std::map<live::TrackId, Spot> spots_;                                 ///< every line, as last drawn
    std::map<std::uint64_t, juce::Rectangle<float>> decoderAt_;           ///< each Decoder's box, as last drawn
    std::map<live::TrackId, std::vector<juce::Rectangle<float>>> chips_;  ///< each chain's insert chips, as drawn
    std::optional<live::TrackId> chosen_;  ///< the node whose wires are open
    ui::Armed<Waiting> armed_;
    std::optional<Menu> menu_;
    std::optional<Moving> moving_;
    ui::ConfirmBox confirm_;
    live::Shape dragging_;  ///< the shape at the press of a level being dragged
    std::vector<std::pair<juce::String, juce::Rectangle<float>>> menuItems_;  ///< the open menu's, as last drawn
};

}  // namespace bambi::livehost
