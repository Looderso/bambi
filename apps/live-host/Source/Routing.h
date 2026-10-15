// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <vector>

#include "bambi/live/field.hpp"
#include "bambi/live/page.hpp"
#include "bambi/ui/ConfirmBox.h"
#include "bambi/ui/HitArea.h"

/*  The live host's routing page: Live's tracks and groups as the page lays them out, the field the shape makes of
 *  them, Main and its chain, and Live's returns below. What it changes is the shape; the host lays the field out
 *  again and routes Live from it.
 *
 *    - every node at the top, and every return: `live | bambi`, the chosen one a block;
 *    - a track inside a group the field takes whole: "encode here", which makes the group a bus; a bus: "encode
 *      whole", which takes it back;
 *    - a node in the field, Main and a return in it: its inserts as chips, each with a cross that removes it after
 *      asking -- the host has no undo -- and "+ echo", "+ reverb" while the pointer is on its row;
 *    - what a row offers only under the pointer is on the line under its box, in place of the sends, so it never
 *      reaches into the next column; a column is as wide as its widest node with its switch and chips.
 *    - under a node, its sends to the returns the field plays, at its own knobs in Live; one Live plays, whose send
 *      reaches a muted return, is marked lost.
 *
 *  A chain on a node the field no longer takes is kept, for when it is taken again. The band at the top stays while
 *  the rest scrolls. Clicks only, on release. Message thread.
 */
namespace bambi::livehost {

class RoutingPage final : public ui::HitArea {
public:
    RoutingPage();

    /// What Live's set is now, and what the field makes of it.
    void show(const live::Tree& tree, const live::Shape& shape, const live::Field& field,
              const std::vector<live::TrackPair>& pairs);

    std::function<void(live::Shape)> reshape;  ///< the user changed the shape
    std::function<void()> showDecoder;         ///< the decoder's node was clicked

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;

private:
    static constexpr live::TrackId kHoverMain = ~live::TrackId{0};  ///< Main's row under the pointer

    void body(juce::Graphics& g, float top);
    float node(juce::Graphics& g, const live::Node& n, juce::Point<float> at);
    float chain(juce::Graphics& g, live::TrackId owner, float x, float y);
    float adders(juce::Graphics& g, live::TrackId owner, float x, float y);
    float controlsWidth(const live::Node& n) const;
    float chainWidth(live::TrackId owner) const;
    void band(juce::Graphics& g);
    const live::Part* partOf(live::TrackId id) const;
    int pairOf(live::TrackId id) const;
    void change(const std::function<void(live::Shape&)>& edit);

    live::Tree tree_;
    live::Shape shape_;
    live::Field field_;
    std::vector<live::TrackPair> pairs_;
    live::PageLayout layout_;
    std::vector<std::pair<juce::Rectangle<float>, live::TrackId>> rows_;  ///< as last drawn, for the pointer
    live::TrackId hover_{0};
    ui::ConfirmBox confirm_;
};

}  // namespace bambi::livehost
