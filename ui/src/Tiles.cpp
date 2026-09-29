// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/ui/Tiles.h"

#include "bambi/ui/Draw.h"
#include "bambi/ui/Widgets.h"

namespace bambi::ui {

namespace {
namespace colour = theme::colour;
namespace ctl = theme::controls;
}  // namespace

float tileHeight(float valueSize) {
    return ctl::tileNameHeight + ctl::tileValueGap + valueSize * ctl::valueLineHeight + ctl::tileValueGap +
           ctl::barHeight;
}

juce::Rectangle<float> tileCell(juce::Rectangle<float> content, float y, int index, float valueSize) {
    const auto columnWidth = (content.getWidth() - ctl::tileColumnGap) / 2.0f;
    const auto height = tileHeight(valueSize);
    return {content.getX() + static_cast<float>(index % 2) * (columnWidth + ctl::tileColumnGap),
            y + static_cast<float>(index / 2) * (height + ctl::tileRowGap), columnWidth, height};
}

float tilesBottom(float y, int count, float valueSize) {
    const auto rows = static_cast<float>((count + 1) / 2);
    return y + rows * (tileHeight(valueSize) + ctl::tileRowGap) - ctl::tileRowGap;
}

juce::Rectangle<float> tileBar(juce::Rectangle<float> tile) { return tile.withTop(tile.getBottom() - ctl::barHeight); }

juce::Rectangle<float> tileBox(juce::Rectangle<float> tile) {
    return tile.expanded(ctl::tileBoxPadX, ctl::tileBoxPadY);
}

juce::Rectangle<float> drawTile(juce::Graphics& g, juce::Rectangle<float> tile, const TileContent& t) {
    const auto box = tileBox(tile);
    if (t.highlighted) {
        g.setColour(colour::highlight);
        g.fillRect(box);
    }
    g.setColour(colour::rule);
    g.drawRect(box, theme::stroke::rule);

    const auto nameFont = labelFont();
    const auto nameColour = !t.enabled ? colour::inactive : (t.nameActive ? colour::text : colour::label);
    text(g, t.name, nameFont, nameColour, tile.withHeight(ctl::tileNameHeight));
    if (t.committed) {
        g.setColour(colour::modulation());
        g.fillRect(tile.getX(), tile.getY() + ctl::tileNameHeight, textWidth(nameFont, t.name),
                   theme::stroke::underline);
    }

    const auto valueTop = tile.getY() + ctl::tileNameHeight + ctl::tileValueGap;
    const auto valueHeight = t.valueSize * ctl::valueLineHeight;
    text(g, t.value, font(t.valueSize),
         !t.enabled ? colour::inactive : (t.modulationInk ? colour::modulation() : colour::text),
         {tile.getX(), valueTop, tile.getWidth(), valueHeight});

    //  The one bar: the tile only says what to show, never how to draw it.
    BarContent bar;
    bar.fillFrom = t.fillFrom;
    bar.fillTo = t.fillTo;
    bar.modFrom = t.modFrom;
    bar.modTo = t.modTo;
    bar.tick = t.tick;
    bar.enabled = t.enabled;
    bar.live = t.live;
    bar.hasLive = t.hasLive;
    bar.ink = t.modulationInk ? colour::modulation() : colour::valueFill;
    drawValueBar(g, tileBar(tile), bar);

    /*  The bar sits flush with the tile's bottom edge, so the mark's overshoot falls outside the tile; a
        caller that registers the tile alone leaves the old mark's foot standing until a full repaint. */
    return t.hasLive ? tileBar(tile).expanded(0.0f, ctl::liveMarkOver) : juce::Rectangle<float>{};
}

double normalised(const ParamManifest& m, ParamId id, double value) {
    return static_cast<double>(bambi::toNormalised(m, static_cast<int>(id), static_cast<float>(value)));
}

}  // namespace bambi::ui
