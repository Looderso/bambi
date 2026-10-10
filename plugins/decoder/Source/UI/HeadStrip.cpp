// SPDX-License-Identifier: GPL-3.0-or-later
#include "HeadStrip.h"

#include "bambi/ui/Draw.h"
#include "bambi/ui/Theme.h"
#include "bambi/ui/Tiles.h"
#include "bambi/ui/Widgets.h"

namespace bambi::ui {

using namespace theme;

HeadStrip::HeadStrip(DecoderControlState& state) : ParameterPage(state), state_(state) { setOpaque(true); }

bool HeadStrip::liveValue(ParamId id, double& out) const {
    const DecoderState& d = state_.decoder();
    if (d.trackerSource == 0 || !d.trackerOverrides) return false;
    const auto& diag = state_.processor.diagnostics();
    if (id == state_.param("head.yaw"))
        out = diag.headYaw.load(std::memory_order_relaxed);
    else if (id == state_.param("head.pitch"))
        out = diag.headPitch.load(std::memory_order_relaxed);
    else if (id == state_.param("head.roll"))
        out = diag.headRoll.load(std::memory_order_relaxed);
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
    float y = content.getY();
    text(g, "head", labelFont(), colour::label, {content.getX(), y, 60.0f, static_cast<float>(space::grid * 2)},
         juce::Justification::centredLeft);
    y += static_cast<float>(space::grid * 2);

    //  what turns the head, and whether it speaks
    const float rowH = static_cast<float>(space::grid * 3);
    const juce::Rectangle<float> seg{content.getX(), y, 264.0f, rowH};
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
    const auto& diag = state_.processor.diagnostics();
    const auto lastMs = diag.trackerLastMs.load(std::memory_order_relaxed);
    const double age = lastMs > 0 ? static_cast<double>(juce::Time::currentTimeMillis() - lastMs) / 1000.0 : 1e9;
    const bool alive = d.trackerSource != 0 && age < 1.0;
    const float dotX = seg.getRight() + static_cast<float>(space::grid) + 2.0f;
    g.setColour(d.trackerSource == 0 ? colour::inactive : alive ? colour::text : colour::rule);
    g.fillRect(dotX, y + rowH / 2.0f - 3.5f, 7.0f, 7.0f);
    juce::String live;
    const juce::String dot = juce::String::fromUTF8("\xc2\xb7"), name = d.trackerSource == 2   ? "airpods"
                                                                        : d.trackerSource == 3 ? "phone"
                                                                                               : "osc";
    if (d.trackerSource == 0)
        live = "no tracker " + dot + " sliders turn the head";
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
    text(g, live, smallFont(), colour::label, {dotX + 12.0f, y, 150.0f, rowH}, juce::Justification::centredLeft);

    //  recentre, setup, overrides
    const float recentreW = framedButtonWidth("recentre"), setupW = framedButtonWidth("setup\xe2\x80\xa6");
    recentre_ = {content.getRight() - recentreW - setupW - 90.0f,
                 y + (rowH - static_cast<float>(space::grid * 2) - 2.0f) / 2.0f, recentreW,
                 static_cast<float>(space::grid * 2) + 2.0f};
    paintButton(g, recentre_, "recentre", d.trackerSource != 0 && mine, [this] { state_.processor.recentre(); });
    setup_ = recentre_.withX(recentre_.getRight() + static_cast<float>(space::grid) / 2.0f).withWidth(setupW);
    paintButton(g, setup_, juce::String::fromUTF8("setup\xe2\x80\xa6"), true,
                [this] { state_.openTemp(TempTab::Tracking); });
    const juce::Rectangle<float> row{setup_.getRight(), y, content.getRight() - setup_.getRight(), rowH};
    const auto overrides = paintSceneToggle(g, row, content.getRight(), "overrides", d.trackerOverrides, mine);
    overrides_ = overrides.hit;
    addClickArea(overrides.hit, [this] {
        model().applyEdit("tracker overrides",
                          [](PluginState& s) { s.decoder.trackerOverrides = !s.decoder.trackerOverrides; });
    });
    y += rowH + static_cast<float>(space::grid);

    //  yaw, pitch, roll: parameters, with the tracker's value as the live mark while it overrides them
    int cell = 0;
    for (const char* key : {"head.yaw", "head.pitch", "head.roll"})
        addParameterTile(g, tileCell(content, y, cell++), state_.param(key), {}, mine);
    y = tilesBottom(y, cell) + static_cast<float>(space::grid) / 2.0f;

    juce::String note;
    //  A field with no direction in it is the one thing to say before anything else: a track too narrow for the
    //  encoder's order hands the decoder W alone, and W is heard as mono from everywhere.
    const int order = state_.processor.engineOrder(), in = diag.mainInputChannels.load(std::memory_order_relaxed);
    if (order == 0 && in > 0)
        note = "the input is " + juce::String(in) + (in == 1 ? " channel" : " channels") + ": order 0, no direction " +
               juce::String::fromUTF8("\xc2\xb7") + " the track needs 4, 9, 16 or more (reaper's master has 2)";
    else if (d.headphonesPlay)
        note = d.throughLayout ? "headphones: the head turns what you hear, through the layout"
                               : "headphones: the head turns what you hear";
    if (d.speakersPlay)
        note += (note.isEmpty() ? "" : "  " + juce::String::fromUTF8("\xc2\xb7") + "  ") +
                juce::String("speakers: the head turns nothing, you are in the room");
    if (note.isEmpty()) note = "nothing plays: the head is shown, and turns nothing";
    text(g, note, smallFont(), colour::label,
         {content.getX(), y, content.getWidth(), static_cast<float>(space::grid * 2)},
         juce::Justification::centredLeft);
}

}  // namespace bambi::ui
