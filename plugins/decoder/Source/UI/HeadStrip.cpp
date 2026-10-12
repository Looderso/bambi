// SPDX-License-Identifier: GPL-3.0-or-later
#include "HeadStrip.h"

#include <algorithm>

#include "bambi/ui/Draw.h"
#include "bambi/ui/Theme.h"
#include "bambi/ui/Tiles.h"
#include "bambi/ui/Widgets.h"

namespace bambi::ui {

using namespace theme;
namespace ctl = theme::controls;

HeadStrip::HeadStrip(DecoderControlState& state) : ParameterPage(state), state_(state) { setOpaque(true); }

bool HeadStrip::liveValue(ParamId id, double& out) const {
    //  the scene's turn in force: the slider, its rate integrated, and the tracker when it turns the scene
    const auto& diag = state_.processor.diagnostics();
    if (id == decoderParam("head.yaw"))
        out = diag.sceneYaw.load(std::memory_order_relaxed);
    else if (id == decoderParam("head.pitch"))
        out = diag.scenePitch.load(std::memory_order_relaxed);
    else if (id == decoderParam("head.roll"))
        out = diag.sceneRoll.load(std::memory_order_relaxed);
    else
        return false;
    return true;
}

void HeadStrip::paint(juce::Graphics& g) {
    clearRegions();
    clearNameAreas();
    const DecoderState& d = state_.decoder();
    const bool mine = !model().showingAnother();
    const auto& diag = state_.processor.diagnostics();
    const juce::String dot = juce::String::fromUTF8("\xc2\xb7");

    //  The tabs: what tracks the head. The tracker's word stands in the bar beside them -- whether it speaks and
    //  how often. A field with no direction in it is the one thing to say before anything else: a track too narrow
    //  for the encoder's order hands the decoder W alone, and W is heard as mono from everywhere.
    const int source = std::clamp(d.trackerSource, 0, 3);
    const bool helper = state_.processor.helperAvailable();  // airpods and the phone's page both need it
    const auto lastMs = diag.trackerLastMs.load(std::memory_order_relaxed);
    const double age = lastMs > 0 ? static_cast<double>(juce::Time::currentTimeMillis() - lastMs) / 1000.0 : 1e9;
    const bool alive = source != 0 && age < 1.0;
    const juce::String name = source == 2 ? "airpods" : source == 3 ? "phone" : "osc";
    const int order = state_.processor.engineOrder(), in = diag.mainInputChannels.load(std::memory_order_relaxed);
    juce::String live;
    if (order == 0 && in > 0)
        live = "the input is " + juce::String(in) + (in == 1 ? " channel" : " channels") + ": order 0, no direction " +
               dot + " the track needs 4, 9, 16 or more (reaper's master has 2)";
    else if (source == 0)
        live = "the sliders and their rates turn the scene";
    else if (alive)
        live = name + " " + dot + " " + juce::String(rateHz_, 0) + " hz";
    else if (lastMs > 0)
        live = name + " " + dot + " silent " + juce::String(age, 1) + " s";
    else if (source == 2) {
        const juce::String word = state_.processor.helperStatus();  // the helper's own, while it starts and asks
        live = word.isEmpty() ? "airpods " + dot + " starting the helper" : word;
    } else if (source == 3) {
        const juce::String word = state_.processor.phoneStatus();
        live = word.isEmpty() ? "phone " + dot + " starting the helper" : "phone " + dot + " " + word;
    } else
        live = "osc " + dot + " port " + juce::String(d.oscPort) + ", nothing yet";
    g.fillAll(colour::ground);
    const float width = static_cast<float>(getWidth()), inset = static_cast<float>(space::inset);
    const std::vector<juce::String> names{"none", "osc", "airpods", "phone"};
    const auto boxes = paintTabRow(g, inset, names, source, [this, helper](int which) {
        if (which >= 2 && !helper) return;  // nothing to read them with: the setup tab says so
        model().applyEdit("tracker", [which](PluginState& s) { s.decoder.trackerSource = which; });
    });
    //  the tracker's word at the right of the row, where the matrix has its count, the dot before it
    const float tabTop = ctl::matrixTabsTop, tabHeight = ctl::tabBarHeight - ctl::matrixTabsTop;
    const float wordLeft = (boxes.empty() ? inset : boxes.back().getRight()) + ctl::tabGap;
    const float wordWidth = std::min(textWidth(labelFont(), live) + 2.0f, width - inset - wordLeft);
    const float wordX = width - inset - wordWidth;
    text(g, live, labelFont(), colour::label, {wordX, tabTop, wordWidth, tabHeight}, juce::Justification::centredRight);
    g.setColour(source == 0 ? colour::inactive : alive ? colour::text : colour::rule);
    g.fillRect(wordX - 12.0f, tabTop + tabHeight / 2.0f - 3.5f, 7.0f, 7.0f);
    const juce::Rectangle<float> content{inset, ctl::tabBarHeight + ctl::segmentGap, width - 2.0f * inset,
                                         static_cast<float>(getHeight()) - ctl::tabBarHeight - ctl::segmentGap};
    float y = content.getY();
    const bool tracked = source != 0;

    //  What the tracker's orientation does -- undone as a head, so what the listener hears stays in the room, or
    //  one more turn of the scene -- and the tracker's buttons; static holds the sliders and rates at zero, to
    //  hear a mix stand still while its turn is automated. Everything a tracker needs greys without one.
    const juce::Rectangle<float> turns{content.getX(), y, 112.0f, ctl::segmentHeight};
    paintSegments(g, turns, {"head", "scene"}, d.trackerTurnsScene ? 1 : 0, mine && tracked, [this](int which) {
        model().applyEdit("tracker turns", [which](PluginState& s) { s.decoder.trackerTurnsScene = which == 1; });
    });
    const juce::Rectangle<float> row{turns.getRight(), y, content.getRight() - turns.getRight(), ctl::segmentHeight};
    const auto still = paintSceneToggle(g, row, content.getRight(), "static", d.sceneStatic, mine);
    if (mine)
        addClickArea(still.hit, [this] {
            model().applyEdit("static", [](PluginState& s) { s.decoder.sceneStatic = !s.decoder.sceneStatic; });
        });
    const float buttonY = y + (ctl::segmentHeight - ctl::buttonHeight) / 2.0f;
    setup_ = {still.hit.getX() - static_cast<float>(space::grid * 2) - framedButtonWidth("setup\xe2\x80\xa6"), buttonY,
              framedButtonWidth("setup\xe2\x80\xa6"), ctl::buttonHeight};
    paintButton(g, setup_, juce::String::fromUTF8("setup\xe2\x80\xa6"), tracked,
                [this] { state_.openTemp(TempTab::Tracking); });
    recentre_ = {setup_.getX() - ctl::segmentGap - framedButtonWidth("recentre"), buttonY,
                 framedButtonWidth("recentre"), ctl::buttonHeight};
    paintButton(g, recentre_, "recentre", tracked && mine, [this] { state_.processor.recentre(); });
    y += ctl::segmentHeight + ctl::segmentGap;

    //  The scene's turn: yaw, pitch and roll, their rates under them -- the strip is wide and short, so the rates
    //  stack under the states instead of standing beside them -- the turn in force as the live mark. In each
    //  angle's name line, the tracker's sign on that axis; the toggle is added after the tile, so it takes the click.
    const float columnW = (content.getWidth() - 2.0f * ctl::tileColumnGap) / 3.0f;
    const float tileH = tileHeight(type::value);
    const auto cell = [&](int column, float top) {
        return juce::Rectangle<float>{content.getX() + static_cast<float>(column) * (columnW + ctl::tileColumnGap), top,
                                      columnW, tileH};
    };
    int column = 0;
    for (const char* key : {"head.yaw", "head.pitch", "head.roll"}) {
        const juce::Rectangle<float> tile = cell(column, y);
        addParameterTile(g, tile, decoderParam(key), {}, mine && !d.sceneStatic);
        const juce::Rectangle<float> nameLine{tile.getX(), tile.getY(), tile.getWidth(), ctl::tileNameHeight};
        const int axis = column;
        const auto flip = paintSceneToggle(g, nameLine, tile.getRight(), "flip", d.flip[static_cast<std::size_t>(axis)],
                                           mine && tracked);
        if (mine && tracked)
            addClickArea(flip.hit, [this, axis] {
                model().applyEdit("flip", [axis](PluginState& s) {
                    s.decoder.flip[static_cast<std::size_t>(axis)] = !s.decoder.flip[static_cast<std::size_t>(axis)];
                });
            });
        ++column;
    }
    y += tileH + ctl::tileRowGap;
    column = 0;
    for (const char* key : {"head.yaw_rate", "head.pitch_rate", "head.roll_rate"})
        addParameterTile(g, cell(column++, y), decoderParam(key), {}, mine && !d.sceneStatic);
}

}  // namespace bambi::ui
