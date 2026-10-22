// SPDX-License-Identifier: GPL-3.0-or-later
#include "HeadStrip.h"

#include <algorithm>
#include <array>

#include "bambi/ui/Draw.h"
#include "bambi/ui/Theme.h"
#include "bambi/ui/Tiles.h"
#include "bambi/ui/Widgets.h"

namespace bambi::ui {

using namespace theme;
namespace ctl = theme::controls;
namespace hdr = theme::header;

HeadStrip::HeadStrip(DecoderControlState& state) : ParameterPage(state), state_(state) { setOpaque(true); }

bool HeadStrip::liveValue(ParamId id, double& out) const {
    //  each axis's own turn: its slider and what its rate has turned, never the tracker or another axis
    const auto& diag = state_.processor.diagnostics();
    if (id == decoderParam("head.yaw"))
        out = diag.turnYaw.load(std::memory_order_relaxed);
    else if (id == decoderParam("head.pitch"))
        out = diag.turnPitch.load(std::memory_order_relaxed);
    else if (id == decoderParam("head.roll"))
        out = diag.turnRoll.load(std::memory_order_relaxed);
    else
        return false;
    return true;
}

void HeadStrip::onDefaultRestored(ParamId id) {
    ParameterPage::onDefaultRestored(id);
    //  an angle set back to zero by hand is at zero: what its rate had turned goes with it
    for (int axis = 0; axis < 3; ++axis)
        if (id == decoderParam(std::array{"head.yaw", "head.pitch", "head.roll"}[static_cast<std::size_t>(axis)]))
            state_.processor.zeroTurn(axis);
}

void HeadStrip::paint(juce::Graphics& g) {
    clearRegions();
    clearNameAreas();
    const DecoderState& d = state_.decoder();
    const bool mine = !model().showingAnother();
    const auto& diag = state_.processor.diagnostics();

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
        live = "the input is " + juce::String(in) + (in == 1 ? " channel" : " channels") + ": order 0, no direction" +
               kSeparator + "the track needs 4, 9, 16 or more (reaper's master has 2)";
    else if (source == 0)
        live = "the sliders and their rates turn the scene";
    else if (alive)
        live = name + kSeparator + juce::String(rateHz_, 0) + " hz";
    else if (lastMs > 0)
        live = name + kSeparator + "silent " + juce::String(age, 1) + " s";
    else if (source == 2) {
        const juce::String word = state_.processor.helperStatus();  // the helper's own, while it starts and asks
        live = word.isEmpty() ? "airpods" + kSeparator + "starting the helper" : word;
    } else if (source == 3) {
        const juce::String word = state_.processor.phoneStatus();
        live = word.isEmpty() ? "phone" + kSeparator + "starting the helper" : "phone" + kSeparator + word;
    } else
        live = "osc" + kSeparator + "port " + juce::String(d.oscPort) + ", nothing yet";
    g.fillAll(colour::ground);
    const float width = static_cast<float>(getWidth()), inset = static_cast<float>(space::inset);
    const std::vector<juce::String> names{"none", "osc", "airpods", "phone"};
    const auto boxes = paintTabRow(
        g, inset, names, source,
        [this](int which) {
            model().applyEdit("tracker", [which](PluginState& s) { s.decoder.trackerSource = which; });
        },
        [helper](int which) { return which < 2 || helper; });  // airpods and the phone need the bundled helper
    //  At the right of the row, where the matrix has its count: the tracker's word with a status square before it,
    //  then recentre and the tracker's setup as a temporary tab, greyed without a tracker.
    const float tabTop = ctl::matrixTabsTop, tabHeight = ctl::tabRowHeight;
    const bool tracked = source != 0;
    //  the buttons as tall as the tabs beside them, so the row is one height
    const juce::String setupLabel = juce::String::fromUTF8("setup\xe2\x80\xa6");
    setup_ = {width - inset - framedButtonWidth(setupLabel), tabTop, framedButtonWidth(setupLabel), tabHeight};
    paintButton(g, setup_, setupLabel, tracked, [this] { state_.openTemp(TempTab::Tracking); });
    recentre_ = {setup_.getX() - ctl::segmentGap - framedButtonWidth("recentre"), tabTop, framedButtonWidth("recentre"),
                 tabHeight};
    paintButton(g, recentre_, "recentre", tracked && mine, [this] { state_.processor.recentre(); });
    const float wordLeft =
        (boxes.empty() ? inset : boxes.back().getRight()) + ctl::tabGap + hdr::statusSquare + hdr::gap / 2.0f;
    const float wordRight = recentre_.getX() - static_cast<float>(space::grid);
    const float wordWidth = std::max(0.0f, std::min(textWidth(labelFont(), live) + 2.0f, wordRight - wordLeft));
    const float wordX = wordRight - wordWidth;
    text(g, live, labelFont(), colour::label, {wordX, tabTop, wordWidth, tabHeight}, juce::Justification::centredRight);
    statusSquare(g, wordX - hdr::gap / 2.0f - hdr::statusSquare, tabTop + tabHeight / 2.0f,
                 source == 0 ? colour::inactive
                 : alive     ? colour::ok
                             : colour::waiting);
    const juce::Rectangle<float> content{inset, ctl::tabBarHeight + ctl::segmentGap, width - 2.0f * inset,
                                         static_cast<float>(getHeight()) - ctl::tabBarHeight - ctl::segmentGap};
    float y = content.getY();

    //  The scene's turn: yaw, pitch and roll, their rates under them -- the strip is wide and short, so the rates
    //  stack under the states instead of standing beside them -- each angle's own turn as the live mark. In each
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
        addParameterTile(g, tile, decoderParam(key), {}, mine);
        const int axis = column++;
        //  a flip changes how a tracker is read: without one it greys, and shows off, since nothing is flipped
        const juce::Rectangle<float> nameLine{tile.getX(), tile.getY(), tile.getWidth(), ctl::tileNameHeight};
        const auto flip = paintSceneToggle(g, nameLine, tile.getRight(), "flip",
                                           tracked && d.flip[static_cast<std::size_t>(axis)], mine && tracked);
        if (mine && tracked)
            addClickArea(flip.hit, [this, axis] {
                model().applyEdit("flip", [axis](PluginState& s) {
                    s.decoder.flip[static_cast<std::size_t>(axis)] = !s.decoder.flip[static_cast<std::size_t>(axis)];
                });
            });
    }
    y += tileH + ctl::tileRowGap;
    column = 0;
    for (const char* key : {"head.yaw_rate", "head.pitch_rate", "head.roll_rate"})
        addParameterTile(g, cell(column++, y), decoderParam(key), {}, mine);
}

}  // namespace bambi::ui
