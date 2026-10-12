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
    if (id == state_.param("head.yaw"))
        out = diag.sceneYaw.load(std::memory_order_relaxed);
    else if (id == state_.param("head.pitch"))
        out = diag.scenePitch.load(std::memory_order_relaxed);
    else if (id == state_.param("head.roll"))
        out = diag.sceneRoll.load(std::memory_order_relaxed);
    else
        return false;
    return true;
}

void HeadStrip::paint(juce::Graphics& g) {
    clearRegions();
    clearNameAreas();
    g.fillAll(colour::ground);
    const auto inset = static_cast<float>(space::inset);
    const juce::Rectangle<float> content{inset, static_cast<float>(space::grid),
                                         static_cast<float>(getWidth()) - 2.0f * inset,
                                         static_cast<float>(getHeight()) - static_cast<float>(space::grid)};
    const DecoderState& d = state_.decoder();
    const bool mine = !model().showingAnother();
    const auto& diag = state_.processor.diagnostics();
    const juce::String dot = juce::String::fromUTF8("\xc2\xb7");
    float y = content.getY();

    //  the heading, and beside it the tracker's word: whether it speaks and how often. A field with no direction
    //  in it is the one thing to say before anything else -- a track too narrow for the encoder's order hands the
    //  decoder W alone, and W is heard as mono from everywhere -- so that takes the line when it applies.
    const float lineH = static_cast<float>(space::grid * 2);
    const float headingW = textWidth(labelFont(), "orientation") + static_cast<float>(space::grid);
    text(g, "orientation", labelFont(), colour::label, {content.getX(), y, headingW, lineH},
         juce::Justification::centredLeft);
    const auto lastMs = diag.trackerLastMs.load(std::memory_order_relaxed);
    const double age = lastMs > 0 ? static_cast<double>(juce::Time::currentTimeMillis() - lastMs) / 1000.0 : 1e9;
    const bool alive = d.trackerSource != 0 && age < 1.0;
    const float dotX = content.getX() + headingW;
    g.setColour(d.trackerSource == 0 ? colour::inactive : alive ? colour::text : colour::rule);
    g.fillRect(dotX, y + lineH / 2.0f - 3.5f, 7.0f, 7.0f);
    juce::String live;
    const juce::String name = d.trackerSource == 2 ? "airpods" : d.trackerSource == 3 ? "phone" : "osc";
    const int order = state_.processor.engineOrder(), in = diag.mainInputChannels.load(std::memory_order_relaxed);
    if (order == 0 && in > 0)
        live = "the input is " + juce::String(in) + (in == 1 ? " channel" : " channels") + ": order 0, no direction " +
               dot + " the track needs 4, 9, 16 or more (reaper's master has 2)";
    else if (d.trackerSource == 0)
        live = "no tracker";
    else if (alive)
        live = name + " " + dot + " " + juce::String(rateHz_, 0) + " hz";
    else if (lastMs > 0)
        live = name + " " + dot + " silent " + juce::String(age, 1) + " s";
    else if (d.trackerSource == 2) {
        const juce::String word = state_.processor.helperStatus();  // the helper's own, while it starts and asks
        live = word.isEmpty() ? "airpods " + dot + " starting the helper" : word;
    } else if (d.trackerSource == 3) {
        const juce::String word = state_.processor.phoneStatus();
        live = word.isEmpty() ? "phone " + dot + " starting the helper" : "phone " + dot + " " + word;
    } else
        live = "osc " + dot + " port " + juce::String(d.oscPort) + ", nothing yet";
    text(g, live, labelFont(), colour::label, {dotX + 12.0f, y, content.getRight() - (dotX + 12.0f), lineH},
         juce::Justification::centredLeft);
    y += lineH + ctl::segmentGap;

    //  what tracks the head, what its orientation does, recentre, the setup tab, and static
    const juce::Rectangle<float> seg{content.getX(), y, 264.0f, ctl::segmentHeight};
    const int source = std::clamp(d.trackerSource, 0, 3);
    const bool helper = state_.processor.helperAvailable();  // airpods and the phone's page both need it
    paintSegments(
        g, seg, {"none", "osc", "airpods", "phone"}, source, mine,
        [this](int which) {
            model().applyEdit("tracker", [which](PluginState& s) { s.decoder.trackerSource = which; });
        },
        helper ? -1 : 2);
    const float quarter = seg.getWidth() / 4.0f;
    sourceCells_.clear();
    for (int i = 0; i < 4; ++i)
        sourceCells_.push_back(seg.withX(seg.getX() + static_cast<float>(i) * quarter).withWidth(quarter));
    //  the tracker turns the head -- what the listener hears stays in the room -- or the scene itself
    const juce::Rectangle<float> turns{seg.getRight() + ctl::segmentGap, y, 112.0f, ctl::segmentHeight};
    paintSegments(g, turns, {"head", "scene"}, d.trackerTurnsScene ? 1 : 0, mine, [this](int which) {
        model().applyEdit("tracker turns", [which](PluginState& s) { s.decoder.trackerTurnsScene = which == 1; });
    });
    const float buttonY = y + (ctl::segmentHeight - ctl::buttonHeight) / 2.0f;
    recentre_ = {turns.getRight() + ctl::segmentGap, buttonY, framedButtonWidth("recentre"), ctl::buttonHeight};
    paintButton(g, recentre_, "recentre", d.trackerSource != 0 && mine, [this] { state_.processor.recentre(); });
    setup_ = recentre_.withX(recentre_.getRight() + ctl::segmentGap).withWidth(framedButtonWidth("setup\xe2\x80\xa6"));
    paintButton(g, setup_, juce::String::fromUTF8("setup\xe2\x80\xa6"), true,
                [this] { state_.openTemp(TempTab::Tracking); });
    //  static: the sliders and rates held at zero, to hear a mix stand still while its turn is automated
    const juce::Rectangle<float> row{setup_.getRight(), y, content.getRight() - setup_.getRight(), ctl::segmentHeight};
    const auto still = paintSceneToggle(g, row, content.getRight(), "static", d.sceneStatic, mine);
    static_ = still.hit;
    if (mine)
        addClickArea(still.hit, [this] {
            model().applyEdit("static", [](PluginState& s) { s.decoder.sceneStatic = !s.decoder.sceneStatic; });
        });
    y += ctl::segmentHeight + ctl::sectionGap;

    //  the scene's turn: yaw, pitch and roll, their rates under them. The strip is wide and short, so the rates
    //  stack under the states instead of standing beside them; the live mark is the turn in force.
    const float columnW = (content.getWidth() - 2.0f * ctl::tileColumnGap) / 3.0f;
    const float tileH = tileHeight(type::value);
    const auto cell = [&](int column, float top) {
        return juce::Rectangle<float>{content.getX() + static_cast<float>(column) * (columnW + ctl::tileColumnGap), top,
                                      columnW, tileH};
    };
    int column = 0;
    for (const char* key : {"head.yaw", "head.pitch", "head.roll"})
        addParameterTile(g, cell(column++, y), state_.param(key), {}, mine && !d.sceneStatic);
    y += tileH + ctl::tileRowGap;
    column = 0;
    for (const char* key : {"head.yaw_rate", "head.pitch_rate", "head.roll_rate"})
        addParameterTile(g, cell(column++, y), state_.param(key), {}, mine && !d.sceneStatic);
}

}  // namespace bambi::ui
