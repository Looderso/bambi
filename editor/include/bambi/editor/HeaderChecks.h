// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "bambi/editor/CheckKit.h"
#include "bambi/editor/PluginEditor.h"
#include "bambi/ui/Draw.h"
#include "bambi/ui/ProductName.h"
#include "bambi/ui/Theme.h"

namespace bambi::editor {

/*  The shared header holds still: its preset box keeps one width whatever the preset's name, the plugin's name is
    written whole in a box as wide in every plugin, and at the narrowest window the two clusters do not meet. */
template <class Processor>
void checkHeaderHoldsStill(Product product) {
    std::printf("\nthe header holds still: the preset's name and the plugin's name move nothing\n");
    Processor proc;
    std::unique_ptr<juce::AudioProcessorEditor> made(proc.createEditor());
    auto* ed = dynamic_cast<PluginEditor*>(made.get());
    if (ed == nullptr) {
        check(false, "the editor is the shared one");
        return;
    }
    auto& header = ed->header();
    //  The header as it paints, with the preset shown as given: what a host would see, drawn off screen.
    const auto paintHeader = [&](const ui::Header::Preset& p) {
        header.setPreset(p);
        juce::Image image(juce::Image::ARGB, std::max(1, header.getWidth()), std::max(1, header.getHeight()), true);
        juce::Graphics g(image);
        header.paintEntireComponent(g, false);
    };

    const juce::String names[] = {"", "a", "warm",
                                  "a preset whose name runs on far past anything the header could show"};
    struct Geometry {
        juce::Rectangle<float> name, back, next;
        bool operator==(const Geometry&) const = default;
    };
    std::vector<Geometry> seen;
    for (const auto& name : names)
        for (const bool modified : {false, true}) {
            paintHeader({name, modified, false, true});
            seen.push_back({header.presetNameArea(), header.presetStepArea(false), header.presetStepArea(true)});
        }
    bool still = true;
    for (const auto& g : seen) still = still && g == seen.front();
    //  Catches: the name's box sized to the name again, which moves the label and both arrows.
    check(still, "an empty, a short and a long name, modified or not, leave the preset box and both arrows in place");

    const auto& font = ui::labelFont();
    const juce::String longName = names[3];
    const auto shown = ui::elided(font, longName, ui::theme::header::presetName - 2.0f, " *");
    //  Catches: a cut name that loses its modified mark, or one cut past the box.
    check(shown.endsWith(" *") && ui::textWidth(font, shown) <= ui::theme::header::presetName - 2.0f,
          "a name too long is cut to the box and keeps its modified mark");

    float widest = 0.0f;
    for (const Product p : ui::kProducts) widest = std::max(widest, ui::textWidth(font, ui::productName(p)));
    paintHeader({"warm", false, false, true});
    //  Catches: the name's box sized to this plugin's name (the header jumps when an instance switches product),
    //  or reserved at a shorter word than the widest, which cuts "path encoder".
    check(std::abs(header.productArea().getWidth() - (widest + 2.0f)) < 0.5f,
          "the plugin's name sits in a box as wide as the widest plugin's name, \"" + ui::productName(product) +
              "\" whole in it");

    //  At the narrowest the window allows, the preset cluster still starts right of the plugin's name.
    if (auto* limits = ed->getConstrainer()) ed->setSize(limits->getMinimumWidth(), limits->getMinimumHeight());
    paintHeader({longName, true, false, true});
    check(header.presetStepArea(false).getX() > header.productArea().getRight(),
          "at the narrowest window the preset cluster clears the plugin's name");
}

}  // namespace bambi::editor
