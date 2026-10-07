// SPDX-License-Identifier: GPL-3.0-or-later
#include "Panel.h"

#include <algorithm>
#include <cmath>

#include "bambi/decode/layoutfile.hpp"
#include "bambi/decode/layouts.hpp"
#include "bambi/ui/Draw.h"
#include "bambi/ui/Theme.h"
#include "bambi/ui/Tiles.h"
#include "bambi/ui/Widgets.h"

namespace bambi::ui {

using namespace theme;
namespace sc = theme::scene;

namespace {
constexpr const char* kPageNames[3] = {"headphones", "speakers", "lens"};
const juce::String kDot = juce::String::fromUTF8("\xc2\xb7");

juce::String rangesOf(const std::vector<int>& channels) {
    juce::String out;
    for (std::size_t i = 0; i < channels.size();) {
        std::size_t j = i;
        while (j + 1 < channels.size() && channels[j + 1] == channels[j] + 1) ++j;
        if (!out.isEmpty()) out += ", ";
        out += i == j ? juce::String(channels[i])
                      : juce::String(channels[i]) + juce::String::fromUTF8("\xe2\x80\x93") + juce::String(channels[j]);
        i = j + 1;
    }
    return out;
}
}  // namespace

DecoderPanel::DecoderPanel(DecoderControlState& state) : ParameterPage(state), state_(state) {}

std::vector<int> DecoderPanel::channelsOf(OutputPage output) const {
    const DecoderState& d = state_.decoder();
    if (output == OutputPage::Headphones) return {d.headphoneChannel, d.headphoneChannel + 1};
    std::vector<int> out;
    for (const Loudspeaker& s : d.layout)
        if (!s.imaginary) out.push_back(s.channel);
    std::sort(out.begin(), out.end());
    return out;
}

void DecoderPanel::paint(juce::Graphics& g) {
    clearRegions();
    liveRegion() = {};
    clearNameAreas();
    layoutButtons_ = {};
    ituCells_.clear();

    //  the tabs: the open output's page, then the temporary tabs in the order they were opened
    const char* page = kPageNames[static_cast<int>(state_.page)];
    const auto select = [this](int index) {
        if (index == state_.shownTab) {
            if (index > 0)
                state_.closeTemp(state_.temps[static_cast<std::size_t>(index - 1)]);  // a second click closes it
            return;
        }
        state_.shownTab = index;
        state_.notify();
    };
    FrameContent frame;
    const bool tracking = std::find(state_.temps.begin(), state_.temps.end(), TempTab::Tracking) != state_.temps.end();
    const bool personalise =
        std::find(state_.temps.begin(), state_.temps.end(), TempTab::Personalise) != state_.temps.end();
    const int shown = std::min(state_.shownTab, static_cast<int>(state_.temps.size()));
    if (tracking && personalise) {
        const bool trackingFirst = state_.temps.front() == TempTab::Tracking;
        frame = trackingFirst ? paintFrame(g, {page, "tracking", "personalise"}, shown, select)
                              : paintFrame(g, {page, "personalise", "tracking"}, shown, select);
    } else if (tracking) {
        frame = paintFrame(g, {page, "tracking"}, shown, select);
    } else if (personalise) {
        frame = paintFrame(g, {page, "personalise"}, shown, select);
    } else {
        frame = paintFrame(g, {page}, 0, select);
    }
    if (frame.content.isEmpty()) return;

    const ContentClip clip(*this, g);
    if (shown == 0) {
        switch (state_.page) {
            case OutputPage::Headphones: paintHeadphones(g, frame.content); break;
            case OutputPage::Speakers: paintSpeakers(g, frame.content); break;
            case OutputPage::Lens: paintLens(g, frame.content); break;
        }
    } else if (state_.temps[static_cast<std::size_t>(shown - 1)] == TempTab::Tracking) {
        paintTracking(g, frame.content);
    } else {
        paintPersonalise(g, frame.content);
    }
}

// ---- the channel map, on every output's page ------------------------------------------------

float DecoderPanel::paintChannels(juce::Graphics& g, juce::Rectangle<float> content, float y, OutputPage output) {
    y = paintGroupTitle(g, content, y, "channels");
    const DecoderState& d = state_.decoder();
    const int bus = state_.processor.diagnostics().outputChannels.load(std::memory_order_relaxed);
    const int shownBus = std::max(bus, 2);
    const std::vector<int> mine = channelsOf(output);
    const std::vector<int> hp = channelsOf(OutputPage::Headphones), sp = channelsOf(OutputPage::Speakers);
    const bool playing = output == OutputPage::Headphones ? d.headphonesPlay
                         : output == OutputPage::Speakers ? d.speakersPlay
                                                          : false;

    //  the bus's channels, marked with what each output writes; this output's own outlined when it is not playing
    const float cell = static_cast<float>(space::grid * 2), gap = 1.0f;
    const int perRow = std::max(1, static_cast<int>((content.getWidth() + gap) / (cell + gap)));
    for (int ch = 1; ch <= shownBus; ++ch) {
        const int i = ch - 1;
        const juce::Rectangle<float> box{content.getX() + static_cast<float>(i % perRow) * (cell + gap),
                                         y + static_cast<float>(i / perRow) * (cell + gap), cell, cell};
        const bool byHp = d.headphonesPlay && std::find(hp.begin(), hp.end(), ch) != hp.end();
        const bool bySp = d.speakersPlay && std::find(sp.begin(), sp.end(), ch) != sp.end();
        const bool own = std::find(mine.begin(), mine.end(), ch) != mine.end();
        g.setColour(byHp ? colour::text : bySp ? colour::label : colour::panel);
        g.fillRect(box);
        if (own && !playing) {
            g.setColour(colour::text);
            g.drawRect(box, stroke::toggle);
        }
        text(g, juce::String(ch), font(type::small - 1.0f), byHp || bySp ? colour::textInverse : colour::label, box,
             juce::Justification::centred);
    }
    y += static_cast<float>((shownBus + perRow - 1) / perRow) * (cell + gap) + static_cast<float>(space::grid) / 2.0f;

    juce::String status;
    const int needs = mine.empty() ? 0 : *std::max_element(mine.begin(), mine.end());
    if (output == OutputPage::Lens)
        status = "later: the lens plays on the loudspeakers' channels";
    else if (mine.empty())
        status = "no layout loaded";
    else if (needs > bus)
        status = juce::String(needs) + " channels needed; the host gives " + juce::String(bus);
    else if (playing)
        status = "playing on " + rangesOf(mine);
    else {
        const std::vector<int>& other = output == OutputPage::Headphones ? sp : hp;
        const bool otherOn = output == OutputPage::Headphones ? d.speakersPlay : d.headphonesPlay;
        bool collide = false;
        for (int c : mine) collide = collide || std::find(other.begin(), other.end(), c) != other.end();
        status = otherOn && collide
                     ? juce::String("would stop ") + (output == OutputPage::Headphones ? "speakers" : "headphones") +
                           ": the same channels"
                     : "free to play";
    }
    text(g, status, smallFont(), needs > bus ? colour::text : colour::label,
         {content.getX(), y, content.getWidth(), static_cast<float>(space::grid * 2)},
         juce::Justification::centredLeft);
    return y + static_cast<float>(space::grid * 2) + static_cast<float>(space::groupGap) / 2.0f;
}

float DecoderPanel::paintStateTile(juce::Graphics& g, juce::Rectangle<float> tile, const juce::String& name,
                                   const juce::String& value, double shown, double low, double high, double step,
                                   const char* key, bool enabled, std::function<void(DecoderState&, double)> set,
                                   std::function<void(DecoderState&)> reset) {
    TileContent t;
    t.name = name;
    t.value = value;
    t.fillTo = std::clamp((shown - low) / (high - low), 0.0, 1.0);
    t.enabled = enabled;
    drawTile(g, tile, t);
    if (enabled)
        addStateDrag(
            tileBox(tile), name.toStdString(), key, shown, high - low, step,
            [set](PluginState& s, double v) { set(s.decoder, v); }, [reset](PluginState& s) { reset(s.decoder); }, {},
            low, high, false, {tileBar(tile), low, high});
    return tile.getBottom();
}

// ---- the pages ------------------------------------------------------------------------------

void DecoderPanel::paintHeadphones(juce::Graphics& g, juce::Rectangle<float> content) {
    const DecoderState& d = state_.decoder();
    const bool mine = !model().showingAnother();
    float y = content.getY();

    //  the pair: which channels, among the bus's
    const int bus = std::max(2, state_.processor.diagnostics().outputChannels.load(std::memory_order_relaxed));
    std::vector<juce::String> pairs;
    for (int c = 1; c + 1 <= bus; c += 2)
        pairs.push_back(juce::String(c) + juce::String::fromUTF8("\xe2\x80\x93") + juce::String(c + 1));
    const int selected = std::clamp((d.headphoneChannel - 1) / 2, 0, static_cast<int>(pairs.size()) - 1);
    y = paintGroupTitle(g, content, y, "pair");
    y = paintPicker(g, content.getX(), y, content.getWidth(), pairs, selected, mine,
                    [this](int which) {
                        model().applyEdit("headphone channels",
                                          [which](PluginState& s) { s.decoder.headphoneChannel = 1 + 2 * which; });
                    })
            .bottom +
        static_cast<float>(space::grid);
    y = paintChannels(g, content, y, OutputPage::Headphones);

    //  the HRTF: the model; the KU100 and a SOFA file later
    y = paintGroupTitle(g, content, y, "hrtf");
    const juce::Rectangle<float> hrtf{content.getX(), y, content.getWidth(), static_cast<float>(space::grid * 3)};
    paintSegments(g, hrtf, {"model", "ku100", "sofa file"}, 0, false, [](int) {}, 1);
    y = hrtf.getBottom() + static_cast<float>(space::grid) / 2.0f;
    text(g, "nobody's head, learned from measured heads " + kDot + " ku100 and a sofa file later", smallFont(),
         colour::label, {content.getX(), y, content.getWidth(), static_cast<float>(space::grid * 2)},
         juce::Justification::centredLeft);
    y += static_cast<float>(space::grid * 2) + static_cast<float>(space::groupGap) / 2.0f;

    //  the listener's two sliders
    y = paintGroupTitle(g, content, y, "your head");
    const auto headTile = tileCell(content, y, 0), earTile = tileCell(content, y, 1);
    paintStateTile(
        g, headTile, "head", juce::String(d.headSize, 2) + " " + juce::String::fromUTF8("\xc3\x97"), d.headSize, 0.75,
        1.3, 0.01, "decoder.head_size", mine, [](DecoderState& s, double v) { s.headSize = static_cast<float>(v); },
        [](DecoderState& s) { s.headSize = 1.0f; });
    paintStateTile(
        g, earTile, "ears", juce::String(d.earShift, 2) + " erb", d.earShift, -2.0, 2.0, 0.01, "decoder.ear_shift",
        mine, [](DecoderState& s, double v) { s.earShift = static_cast<float>(v); },
        [](DecoderState& s) { s.earShift = 0.0f; });
    y = tilesBottom(y, 2) + static_cast<float>(space::grid) / 2.0f;
    const float buttonH = static_cast<float>(space::grid * 2) + 2.0f;
    const juce::Rectangle<float> personalise{content.getX(), y, framedButtonWidth("personalise\xe2\x80\xa6"), buttonH};
    paintButton(g, personalise, juce::String::fromUTF8("personalise\xe2\x80\xa6"), true,
                [this] { state_.openTemp(TempTab::Personalise); });
    text(g, "against a loudspeaker " + kDot + " later", smallFont(), colour::inactive,
         {personalise.getRight() + static_cast<float>(space::grid), y, 200.0f, buttonH},
         juce::Justification::centredLeft);
    y += buttonH + static_cast<float>(space::groupGap) / 2.0f;

    //  listen through the layout
    y = paintGroupTitle(g, content, y, "listen");
    const juce::Rectangle<float> row{content.getX(), y, content.getWidth(), static_cast<float>(space::grid * 2)};
    const auto through =
        paintSceneToggle(g, row, content.getX() + sceneToggleWidth("through the layout") + 2.0f * sc::toggleBox,
                         "through the layout", d.throughLayout, mine && !d.layout.empty());
    if (mine && !d.layout.empty())
        addClickArea(through.hit, [this] {
            model().applyEdit("through the layout",
                              [](PluginState& s) { s.decoder.throughLayout = !s.decoder.throughLayout; });
        });
    text(g, d.layoutName.empty() ? juce::String("no layout loaded") : juce::String(d.layoutName), smallFont(),
         colour::label, {through.hit.getRight() + static_cast<float>(space::grid), y, 150.0f, row.getHeight()},
         juce::Justification::centredLeft);
    y += row.getHeight() + 2.0f;
    text(g, "the same layout the speakers page holds, heard on headphones as points", smallFont(), colour::label,
         {content.getX(), y, content.getWidth(), static_cast<float>(space::grid * 2)},
         juce::Justification::centredLeft);
    y += static_cast<float>(space::grid * 2) + static_cast<float>(space::groupGap) / 2.0f;

    y = paintGroupTitle(g, content, y, "headphone correction");
    text(g, "a list of headphones " + kDot + " later", smallFont(), colour::inactive,
         {content.getX(), y, content.getWidth(), static_cast<float>(space::grid * 2)},
         juce::Justification::centredLeft);
    noteContentBottom(y + static_cast<float>(space::grid * 3));
}

void DecoderPanel::chooseLayoutFile() {
    chooser_ =
        std::make_unique<juce::FileChooser>("a loudspeaker layout (IEM AllRADecoder JSON)", juce::File(), "*.json");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this](const juce::FileChooser& fc) {
                              const juce::File file = fc.getResult();
                              if (!file.existsAsFile()) return;
                              Layout layout;
                              std::string error;
                              if (!parseIemLayout(file.loadFileAsString().toStdString(), layout, &error)) return;
                              const std::string name = file.getFileNameWithoutExtension().toStdString();
                              model().applyEdit("load layout", [layout, name](PluginState& s) {
                                  s.decoder.layout = layout.speakers;
                                  s.decoder.layoutName = name;
                              });
                          });
}

void DecoderPanel::paintSpeakers(juce::Graphics& g, juce::Rectangle<float> content) {
    const DecoderState& d = state_.decoder();
    const bool mine = !model().showingAnother();
    float y = content.getY();

    //  the layout: named, loaded from a file, one of the ITU's, typed in later
    y = paintGroupTitle(g, content, y, "layout");
    text(g, d.layoutName.empty() ? juce::String("none") : juce::String(d.layoutName), font(type::body), colour::text,
         {content.getX(), y, content.getWidth(), static_cast<float>(space::grid * 2) + 2.0f},
         juce::Justification::centredLeft);
    y += static_cast<float>(space::grid * 2) + 2.0f + static_cast<float>(space::grid) / 2.0f;
    const float buttonH = static_cast<float>(space::grid * 2) + 2.0f;
    float x = content.getX();
    const char* labels[3] = {"load\xe2\x80\xa6", "itu \xe2\x96\xbe", "type\xe2\x80\xa6"};
    for (int i = 0; i < 3; ++i) {
        const juce::String label = juce::String::fromUTF8(labels[i]);
        layoutButtons_[static_cast<std::size_t>(i)] = {x, y, framedButtonWidth(label), buttonH};
        const bool enabled = mine && i < 2;
        paintButton(g, layoutButtons_[static_cast<std::size_t>(i)], label, enabled, [this, i] {
            if (i == 0) chooseLayoutFile();
            if (i == 1) {
                ituOpen_ = !ituOpen_;
                repaint();
            }
        });
        x += layoutButtons_[static_cast<std::size_t>(i)].getWidth() + static_cast<float>(space::grid) / 2.0f;
    }
    y += buttonH + static_cast<float>(space::grid) / 2.0f;
    if (ituOpen_) {
        std::vector<juce::String> names;
        int selected = -1;
        for (std::string_view n : bs2051Names()) {
            if (n == d.layoutName) selected = static_cast<int>(names.size());
            names.push_back(juce::String(std::string(n)));
        }
        const auto picked = paintPicker(
            g, content.getX(), y, content.getWidth(), names, selected, mine,
            [this](int which) {
                const std::string name(bs2051Names()[static_cast<std::size_t>(which)]);
                const Layout* l = bs2051Layout(name);
                if (l == nullptr) return;
                const std::vector<Loudspeaker> speakers = l->speakers;
                model().applyEdit("layout", [speakers, name](PluginState& s) {
                    s.decoder.layout = speakers;
                    s.decoder.layoutName = name;
                });
                ituOpen_ = false;
            },
            {}, 0, 5);  // the stereo pair is first in the list and not a layout this decoder plays yet
        ituCells_ = picked.cells;
        y = picked.bottom + static_cast<float>(space::grid) / 2.0f;
    }
    const juce::String error = state_.processor.designError();
    if (!error.isEmpty()) {
        text(g, "refused: " + error, smallFont(), colour::text,
             {content.getX(), y, content.getWidth(), static_cast<float>(space::grid * 2)},
             juce::Justification::centredLeft);
        y += static_cast<float>(space::grid * 2);
    }
    text(g, "place by looking " + kDot + " later", smallFont(), colour::inactive,
         {content.getX(), y, content.getWidth(), static_cast<float>(space::grid * 2)},
         juce::Justification::centredLeft);
    y += static_cast<float>(space::grid * 2) + static_cast<float>(space::groupGap) / 2.0f;

    y = paintChannels(g, content, y, OutputPage::Speakers);

    //  the listening situation
    y = paintGroupTitle(g, content, y, "listening");
    const juce::Rectangle<float> seg{content.getX(), y, content.getWidth(), static_cast<float>(space::grid * 3)};
    paintSegments(g, seg, {"one listener", "audience"}, d.weighting == 1 ? 1 : 0, mine, [this](int which) {
        model().applyEdit("weighting", [which](PluginState& s) { s.decoder.weighting = which; });
    });
    y = seg.getBottom() + static_cast<float>(space::grid);
    const juce::Rectangle<float> row{content.getX(), y, content.getWidth(), static_cast<float>(space::grid * 2)};
    const auto distance =
        paintSceneToggle(g, row, content.getX() + sceneToggleWidth("distance compensation") + 2.0f * sc::toggleBox,
                         "distance compensation", d.distanceCompensation, mine);
    if (mine)
        addClickArea(distance.hit, [this] {
            model().applyEdit("distance compensation",
                              [](PluginState& s) { s.decoder.distanceCompensation = !s.decoder.distanceCompensation; });
        });
    y += row.getHeight() + 2.0f;
    const int order = state_.processor.engineOrder();
    text(g,
         "order " + juce::String(std::max(order, 0)) + " " + kDot + " the hull is on the scene while this page is open",
         smallFont(), colour::label, {content.getX(), y, content.getWidth(), static_cast<float>(space::grid * 2)},
         juce::Justification::centredLeft);
    y += static_cast<float>(space::grid * 2) + static_cast<float>(space::groupGap) / 2.0f;

    //  the loudspeakers, one row each
    y = paintGroupTitle(g, content, y, "loudspeakers");
    const float rowH = static_cast<float>(space::grid * 2) + 4.0f;
    const float colW[6] = {30.0f, 48.0f, 46.0f, 56.0f, 58.0f, 62.0f};
    const char* heads[6] = {"ch", "az", "el", "dist m", "gain db", "delay ms"};
    float cx = content.getX();
    for (int c = 0; c < 6; ++c) {
        text(g, heads[c], smallFont(), colour::label, {cx, y, colW[c], rowH},
             c == 0 ? juce::Justification::centredLeft : juce::Justification::centredRight);
        cx += colW[c];
    }
    y += rowH;
    ruleH(g, content.getX(), content.getRight(), y, colour::text);
    y += 2.0f;
    double farthest = 0.0;
    for (const Loudspeaker& s : d.layout)
        if (!s.imaginary) farthest = std::max(farthest, s.radiusM);
    for (const Loudspeaker& s : d.layout) {
        const juce::Colour ink = s.imaginary ? colour::label : colour::text;
        const double gainDb =
            !d.distanceCompensation || s.imaginary || farthest <= 0.0 ? 0.0 : 20.0 * std::log10(s.radiusM / farthest);
        const double delayMs = !d.distanceCompensation || s.imaginary ? 0.0 : (farthest - s.radiusM) / 343.0 * 1000.0;
        const juce::String cells[6] = {s.imaginary ? juce::String("imag") : juce::String(s.channel),
                                       juce::String(s.azimuthDeg, 1),
                                       juce::String(s.elevationDeg, 1),
                                       juce::String(s.radiusM, 2),
                                       s.imaginary ? juce::String::fromUTF8("\xe2\x80\x93") : juce::String(gainDb, 1),
                                       s.imaginary ? juce::String::fromUTF8("\xe2\x80\x93") : juce::String(delayMs, 2)};
        cx = content.getX();
        for (int c = 0; c < 6; ++c) {
            text(g, cells[c], font(type::matrix), ink, {cx, y, colW[c], rowH},
                 c == 0 ? juce::Justification::centredLeft : juce::Justification::centredRight);
            cx += colW[c];
        }
        y += rowH;
        ruleH(g, content.getX(), content.getRight(), y, colour::rowDivider);
    }
    if (d.layout.empty())
        text(g, "load a layout, or pick one of the itu's", smallFont(), colour::label,
             {content.getX(), y, content.getWidth(), rowH}, juce::Justification::centredLeft),
            y += rowH;
    y += static_cast<float>(space::groupGap) / 2.0f;

    y = paintGroupTitle(g, content, y, "later");
    text(
        g,
        "colour correction " + kDot + " bass management " + kDot + " weak spots " + kDot + " test tone per loudspeaker",
        smallFont(), colour::inactive, {content.getX(), y, content.getWidth(), static_cast<float>(space::grid * 2)},
        juce::Justification::centredLeft);
    noteContentBottom(y + static_cast<float>(space::grid * 3));
}

void DecoderPanel::paintLens(juce::Graphics& g, juce::Rectangle<float> content) {
    float y = paintChannels(g, content, content.getY(), OutputPage::Lens);
    y = paintGroupTitle(g, content, y, "slice");
    text(g,
         "one slice of the scene on the loudspeakers: whatever the head points at. diagnostic, not a simulation of the "
         "headphones " +
             kDot + " later",
         smallFont(), colour::inactive, {content.getX(), y, content.getWidth(), static_cast<float>(space::grid * 4)},
         juce::Justification::topLeft);
    noteContentBottom(y + static_cast<float>(space::grid * 5));
}

void DecoderPanel::paintTracking(juce::Graphics& g, juce::Rectangle<float> content) {
    const DecoderState& d = state_.decoder();
    const bool mine = !model().showingAnother();
    float y = content.getY();
    text(g, "the head serves every output: its setup is a temporary tab, opened from the head strip.", smallFont(),
         colour::label, {content.getX(), y, content.getWidth(), static_cast<float>(space::grid * 2)},
         juce::Justification::centredLeft);
    y += static_cast<float>(space::grid * 3);

    y = paintGroupTitle(g, content, y, "osc");
    const auto portTile = tileCell(content, y, 0);
    paintStateTile(
        g, portTile, "port", juce::String(d.oscPort), d.oscPort, 1024, 65535, 1.0, "decoder.osc_port", mine,
        [](DecoderState& s, double v) { s.oscPort = static_cast<int>(std::lround(v)); },
        [](DecoderState& s) { s.oscPort = 9000; });
    y = tilesBottom(y, 1) + static_cast<float>(space::grid) / 2.0f;
    const juce::Rectangle<float> row{content.getX(), y, content.getWidth(), static_cast<float>(space::grid * 2)};
    const auto network =
        paintSceneToggle(g, row, content.getX() + sceneToggleWidth("open to the network") + 2.0f * sc::toggleBox,
                         "open to the network", d.oscNetwork, mine);
    if (mine)
        addClickArea(network.hit, [this] {
            model().applyEdit("osc network", [](PluginState& s) { s.decoder.oscNetwork = !s.decoder.oscNetwork; });
        });
    y += row.getHeight() + 2.0f;
    text(g,
         "format: auto " + kDot + " adm-osc, sparta and iem, by their addresses " + kDot +
             " learn and a phone's page later",
         smallFont(), colour::label, {content.getX(), y, content.getWidth(), static_cast<float>(space::grid * 2)},
         juce::Justification::centredLeft);
    y += static_cast<float>(space::grid * 2) + static_cast<float>(space::groupGap) / 2.0f;

    y = paintGroupTitle(g, content, y, "signs");
    float x = content.getX();
    const char* names[3] = {"flip yaw", "flip pitch", "flip roll"};
    for (int i = 0; i < 3; ++i) {
        const juce::Rectangle<float> r{x, y, content.getRight() - x, static_cast<float>(space::grid * 2)};
        const auto flip = paintSceneToggle(g, r, x + sceneToggleWidth(names[i]) + 2.0f * sc::toggleBox, names[i],
                                           d.flip[static_cast<std::size_t>(i)], mine);
        if (mine)
            addClickArea(flip.hit, [this, i] {
                model().applyEdit("flip", [i](PluginState& s) {
                    s.decoder.flip[static_cast<std::size_t>(i)] = !s.decoder.flip[static_cast<std::size_t>(i)];
                });
            });
        x = flip.hit.getRight() + static_cast<float>(space::grid);
    }
    y += static_cast<float>(space::grid * 2) + static_cast<float>(space::groupGap) / 2.0f;

    y = paintGroupTitle(g, content, y, "airpods");
    const juce::String word = state_.processor.helperStatus();
    text(g,
         state_.processor.helperAvailable()
             ? (d.trackerSource == 2
                    ? (word.isEmpty() ? juce::String("the helper is starting") : word)
                    : juce::String(
                          "choose airpods in the head strip: the helper in this bundle reads them, and asks once"))
             : juce::String("the helper is not in this bundle; macOS only"),
         smallFont(), colour::label, {content.getX(), y, content.getWidth(), static_cast<float>(space::grid * 3)},
         juce::Justification::topLeft);
    y += static_cast<float>(space::grid * 3) + static_cast<float>(space::groupGap) / 2.0f;
    y = paintGroupTitle(g, content, y, "phone");
    text(g, "a page any phone opens " + kDot + " later", smallFont(), colour::inactive,
         {content.getX(), y, content.getWidth(), static_cast<float>(space::grid * 2)},
         juce::Justification::centredLeft);
    noteContentBottom(y + static_cast<float>(space::grid * 3));
}

void DecoderPanel::paintPersonalise(juce::Graphics& g, juce::Rectangle<float> content) {
    float y = content.getY();
    text(g,
         "a temporary tab, entered on purpose, closed when done. a real loudspeaker and a virtual one alternate; turn "
         "and nod; move each slider until the two sit in one place.",
         smallFont(), colour::label, {content.getX(), y, content.getWidth(), static_cast<float>(space::grid * 5)},
         juce::Justification::topLeft);
    y += static_cast<float>(space::grid * 6);
    text(g,
         "the procedure arrives with personalisation " + kDot +
             " later; until then the two sliders on the headphones page",
         smallFont(), colour::inactive, {content.getX(), y, content.getWidth(), static_cast<float>(space::grid * 3)},
         juce::Justification::topLeft);
    noteContentBottom(y + static_cast<float>(space::grid * 4));
}

}  // namespace bambi::ui
