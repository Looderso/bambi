// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "bambi/live/field.hpp"
#include "bambi/live/page.hpp"
#include "bambi/ui/ConfirmBox.h"
#include "bambi/ui/HitArea.h"

/*  The live host's routing page: where everything the host plays sends its audio. Three columns, wires between
 *  them: Live's tracks and groups, as Live lists them, then the host's own inputs; Live's returns and Main; the
 *  Decoders, each with what it hears. What it changes is the shape; the host lays the field out again and routes
 *  Live from it.
 *
 *    - a node's line: Live's colour, its name, a marker -- live, bambi, bus, in group -- the Encoder as a chip, or
 *      "+ encoder", which takes it to bambi; its inserts as chips, a click opening one and a drag along the chain
 *      moving it; "+", a menu of the inserts to add; "send". The
 *      Encoder's chip opens it, its cross gives the node back to Live; a bus offers "encode whole". Every node starts
 *      in Live; "route all", in the band, takes each at the top, and "all to Live" gives every one back;
 *    - a return: the same, its marker choosing whether the field plays it; Main: its chain, "+" and "send";
 *    - a Decoder: its name, a cross that removes it after asking -- never the last -- what it hears, each at its
 *      level, a node heard twice marked; "+ decoder" adds a listener. A click on its name shows it;
 *    - "send" arms: everything it may be wired to is ringed, the rest greyed; a click on one wires it there at 0 dB,
 *      and escape, empty ground or "send" again cancels;
 *    - the wires, drawn from each node's send to where it plays, a level that is not 0 dB on them; a click on a
 *      node's name chooses it, lighting everything it reaches, and its wires open under its line as chips: drag one
 *      sideways for its level, shift finer, double click for 0 dB; its cross removes the wire.
 *
 *  A source's Encoder and every insert carry a sidechain slot: "sc -", or the track it hears. A click arms it --
 *  drawn as something waiting, "sc none" -- and greys everything that cannot be its source; a click on a source then
 *  picks it, and escape, empty ground or the slot again cancels; "none" clears it. A sidechain whose track has left
 *  the field reads lost.
 *
 *  A chain whose track Live no longer has is listed under "orphaned", its inserts removable one by one: kept, never
 *  dropped unseen. "attach" arms it, and a click on a part of the field, on Main or on a return the field plays
 *  moves the whole chain there, after that part's own inserts. A chain on a node the field no longer takes is kept,
 *  for when it is taken again.
 *
 *  The band stays while the rest scrolls. Clicks on release; a level is dragged. Message thread.
 */
namespace bambi::livehost {

class RoutingPage final : public ui::HitArea {
public:
    RoutingPage();

    /// What Live's set is now, and what the field makes of it.
    void show(const live::Tree& tree, const live::Shape& shape, const live::Field& field,
              const std::vector<live::TrackPair>& pairs);

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

    void band(juce::Graphics& g);
    void body(juce::Graphics& g, float top);
    float liveLine(juce::Graphics& g, const live::Node& n, int depth, float x, float y, float width);
    float inputLine(juce::Graphics& g, const live::HostInput& in, float x, float y, float width);
    float returnLine(juce::Graphics& g, const live::Node& r, float x, float y, float width);
    float mainLine(juce::Graphics& g, float x, float y, float width);
    float decoderBox(juce::Graphics& g, std::size_t k, float x, float y, float width);
    float orphans(juce::Graphics& g, float y, float width);
    //  A line's frame, its stripe and name; the box, and where its controls start.
    juce::Rectangle<float> frame(juce::Graphics& g, juce::Rectangle<float> box, const juce::String& name,
                                 juce::Colour stripe, bool inField, live::TrackId id);
    float encoderChip(juce::Graphics& g, live::TrackId id, float x, float y, bool removable);
    float chain(juce::Graphics& g, live::TrackId owner, float x, float y);
    float plus(juce::Graphics& g, live::TrackId owner, float x, float y);
    void sendButton(juce::Graphics& g, live::TrackId from, juce::Rectangle<float> box);
    float wireChips(juce::Graphics& g, live::TrackId from, float x, float y, float width);
    void wires(juce::Graphics& g);
    void menu(juce::Graphics& g);
    //  Whether the node "send" waits for may be wired to `to`.
    bool targetable(const live::Target& to) const;
    bool idle() const { return !pick_.armed() && !sending_; }  ///< nothing waits for a click
    //  While something waits: whether the line of `node` -- kMain for Main -- answers it, and the answer.
    bool takes(live::TrackId node) const;
    void take(live::TrackId node);
    //  A line's end, once it is drawn: ringed and answering while something waits that it takes, else greyed.
    void armedOver(juce::Graphics& g, juce::Rectangle<float> box, live::TrackId node);
    float chainWidth(live::TrackId owner) const;
    float slotWidth(live::TrackId heard, bool waiting) const;

    juce::String nameOf(live::TrackId id) const;
    juce::String nameOf(const live::Target& t) const;
    const live::Part* partOf(live::TrackId id) const;
    int pairOf(live::TrackId id) const;
    void change(const std::function<void(live::Shape&)>& edit);
    void addInsert(live::TrackId owner, live::Effect effect);
    std::function<void()> removal(live::TrackId owner, std::uint64_t id, juce::String name);

    //  A sidechain slot waiting for a source: a source's Encoder's, or an insert's; or an orphaned chain waiting
    //  for a part of the field to take it.
    struct Pick {
        live::TrackId source{0};
        std::uint64_t insert{0};
        live::TrackId orphan{0};  ///< the chain's former track
        bool armed() const { return source != 0 || insert != 0 || orphan != 0; }
    };
    bool canBeSource(live::TrackId id) const;
    juce::String slotLabel(live::TrackId heard) const;
    float slot(juce::Graphics& g, float x, float y, live::TrackId heard, bool waiting, std::function<void()> arm,
               std::function<void()> clear);
    void arm(Pick pick);
    void pickSource(live::TrackId from);
    void attachTo(live::TrackId target);
    juce::String waiting() const;  ///< what the band says while something waits for a click
    void cancel();                 ///< whatever waits, and the menu, let go of

    live::Tree tree_;
    live::Shape shape_;
    live::Field field_;
    std::vector<live::TrackPair> pairs_;
    std::map<live::TrackId, Spot> spots_;               ///< every part, return and Main (kMain), as last drawn
    std::map<std::uint64_t, juce::Rectangle<float>> decoderAt_;  ///< each Decoder's box, as last drawn
    std::map<live::TrackId, std::vector<juce::Rectangle<float>>> chips_;  ///< each chain's insert chips, as drawn
    //  an insert chip being dragged along its chain: whose, which, and where the pointer is
    struct Moving {
        live::TrackId owner{0};
        std::uint64_t id{0};
        float x{0.0f};
    };
    std::optional<Moving> moving_;
    std::size_t placeAt(live::TrackId owner, float x) const;  ///< the place in `owner`'s chain at `x`
    std::optional<live::TrackId> chosen_;  ///< the node whose wires are open
    std::optional<live::TrackId> sending_;  ///< "send" pressed: the node a click wires
    std::optional<live::TrackId> menuFor_;  ///< "+" pressed: the node its menu adds to
    juce::Point<float> menuAt_;
    ui::ConfirmBox confirm_;
    Pick pick_;
    //  a wire's level being dragged: the shape as it was at the press
    live::Shape dragging_;
};

}  // namespace bambi::livehost
