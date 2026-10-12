// SPDX-License-Identifier: GPL-3.0-or-later
#include "Panel.h"

#include <algorithm>
#include <cmath>

#include "../HelperLink.h"
#include "bambi/decode/layoutfile.hpp"
#include "bambi/decode/layouts.hpp"
#include "bambi/decode/outputs.hpp"
#include "bambi/ui/Draw.h"
#include "bambi/ui/Theme.h"
#include "bambi/ui/Tiles.h"
#include "bambi/ui/Widgets.h"
#include "qrcodegen.hpp"

namespace bambi::ui {

using namespace theme;
namespace sc = theme::scene;
namespace ctl = theme::controls;

namespace {
constexpr const char* kPageNames[2] = {"headphones", "speakers"};
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

void DecoderPanel::paint(juce::Graphics& g) {
    clearRegions();
    liveRegion() = {};
    clearNameAreas();

    //  the tabs: the open output's page, then the temporary tabs in the order they were opened
    const auto tabName = [](TempTab t) -> const char* {
        switch (t) {
            case TempTab::Tracking: return "tracking";
            case TempTab::Personalise: return "personalise";
            case TempTab::Loudspeakers: return "loudspeakers";
        }
        return "";
    };
    const int temps = static_cast<int>(state_.temps.size());
    const int shown = std::min(state_.shownTab, temps);
    const auto select = [this](int) {
        state_.shownTab = 0;
        state_.notify();
    };
    //  the temporary tabs after the page's: each a tab with a cross that closes it
    const FrameContent frame = paintFrame(
        g, {kPageNames[static_cast<int>(state_.page)]}, shown == 0 ? 0 : -1, select,
        [this, shown, tabName](juce::Graphics& gg, float x) {
            const auto tabFont = font(type::body, type::trackingTab);
            for (std::size_t i = 0; i < state_.temps.size(); ++i) {
                const TempTab temp = state_.temps[i];
                const juce::String name = tabName(temp);
                const juce::Rectangle<float> box{x, 0.0f, textWidth(tabFont, name) + 2.0f * ctl::tabPadX,
                                                 ctl::tabBarHeight};
                drawChoice(gg, box, name, tabFont, shown == static_cast<int>(i) + 1);
                const int index = static_cast<int>(i) + 1;
                addRegion({box,
                           {},
                           {},
                           {},
                           [this, index] {
                               state_.shownTab = index;
                               state_.notify();
                           },
                           {}});
                const juce::Rectangle<float> cross{box.getRight() + ctl::closeGap,
                                                   (ctl::tabBarHeight - ctl::closeGlyph) / 2.0f, ctl::closeGlyph,
                                                   ctl::closeGlyph};
                addRegion({drawCloseCross(gg, cross), {}, {}, {}, [this, temp] { state_.closeTemp(temp); }, {}});
                x = cross.getRight() + ctl::closeGap + ctl::tabGap + ctl::tabPadX;
            }
            return x;
        });
    if (frame.content.isEmpty()) return;

    const ContentClip clip(*this, g);
    if (shown == 0) {
        switch (state_.page) {
            case OutputPage::Headphones: paintHeadphones(g, frame.content); break;
            case OutputPage::Speakers: paintSpeakers(g, frame.content); break;
        }
        return;
    }
    switch (state_.temps[static_cast<std::size_t>(shown - 1)]) {
        case TempTab::Tracking: paintTracking(g, frame.content); break;
        case TempTab::Personalise: paintPersonalise(g, frame.content); break;
        case TempTab::Loudspeakers: paintLoudspeakers(g, frame.content); break;
    }
}

// ---- the channel map, on every output's page ------------------------------------------------

float DecoderPanel::paintChannels(juce::Graphics& g, juce::Rectangle<float> content, float y, OutputPage output) {
    y = paintGroupTitle(g, content, y, "channels");
    const DecoderState& d = state_.decoder();
    const int bus = state_.processor.getChannelCountOfBus(false, 0);
    const int shownBus = std::max(bus, 2);
    const std::vector<int> mine =
        outputChannels(d, output == OutputPage::Headphones ? Output::Headphones : Output::Speakers);
    const std::vector<int> hp = outputChannels(d, Output::Headphones), sp = outputChannels(d, Output::Speakers);
    const bool playing = output == OutputPage::Headphones ? d.headphonesPlay : d.speakersPlay;

    if (output == OutputPage::Headphones) {
        //  which pair of the bus's channels: the one choice, shown once
        std::vector<juce::String> pairs;
        for (int c = 1; c + 1 <= shownBus; c += 2)
            pairs.push_back(juce::String(c) + juce::String::fromUTF8("\xe2\x80\x93") + juce::String(c + 1));
        const int selected = std::clamp((d.headphoneChannel - 1) / 2, 0, static_cast<int>(pairs.size()) - 1);
        y = paintPicker(g, content.getX(), y, content.getWidth(), pairs, selected, !model().showingAnother(),
                        [this](int which) {
                            model().applyEdit("headphone channels",
                                              [which](PluginState& s) { s.decoder.headphoneChannel = 1 + 2 * which; });
                        })
                .bottom;
    } else {
        //  where the layout starts on the bus: its channels move as a block, so a pair of headphone channels or an
        //  interface's own outputs can sit before them
        int span = 0;
        for (int c : mine) span = std::max(span, c - (std::max(1, d.speakerFirstChannel) - 1));
        const int lastStart = std::max(1, shownBus - span + 1);
        paintStateTile(
            g, tileCell(content, y, 0), "from channel", juce::String(d.speakerFirstChannel), d.speakerFirstChannel, 1.0,
            static_cast<double>(std::max(lastStart, d.speakerFirstChannel)), 1.0, "decoder.speaker_first_channel",
            !model().showingAnother() && !mine.empty(),
            [](DecoderState& s, double v) { s.speakerFirstChannel = std::max(1, static_cast<int>(std::lround(v))); },
            [](DecoderState& s) { s.speakerFirstChannel = 1; });
        y = tilesBottom(y, 1) + ctl::sectionGap;
        //  the bus's channels, marked with what each output writes
        const float cell = static_cast<float>(space::grid * 2), gap = 1.0f;
        const int perRow = std::max(1, static_cast<int>((content.getWidth() + gap) / (cell + gap)));
        for (int ch = 1; ch <= shownBus; ++ch) {
            const int i = ch - 1;
            const juce::Rectangle<float> box{content.getX() + static_cast<float>(i % perRow) * (cell + gap),
                                             y + static_cast<float>(i / perRow) * (cell + gap), cell, cell};
            const bool byHp = d.headphonesPlay && std::find(hp.begin(), hp.end(), ch) != hp.end();
            const bool bySp = d.speakersPlay && std::find(sp.begin(), sp.end(), ch) != sp.end();
            const bool own = std::find(mine.begin(), mine.end(), ch) != mine.end();
            g.setColour(byHp ? colour::text : colour::panel);
            g.fillRect(box);
            g.setColour(colour::text);
            if (bySp) g.drawRect(box, stroke::toggle);
            if (own && !playing) {
                //  this output's own, not playing: dashed, as the scene draws what is not placed
                juce::Path edge, dashed;
                edge.addRectangle(box.reduced(stroke::toggle / 2.0f));
                juce::PathStrokeType(stroke::toggle).createDashedStroke(dashed, edge, stroke::unplacedDash, 2);
                g.fillPath(dashed);
            }
            text(g, juce::String(ch), smallFont(),
                 byHp   ? colour::textInverse
                 : bySp ? colour::text
                        : colour::label,
                 box, juce::Justification::centred);
        }
        y += static_cast<float>((shownBus + perRow - 1) / perRow) * (cell + gap) + ctl::segmentGap;
        //  the key: which ink is whose
        const float lineH = noteLineHeight();
        float kx = content.getX();
        for (const auto& [filled, who] : {std::pair{true, "headphones"}, std::pair{false, "speakers"}}) {
            const juce::Rectangle<float> swatch{kx, y + (lineH - sc::toggleBox) / 2.0f, sc::toggleBox, sc::toggleBox};
            g.setColour(colour::text);
            if (filled)
                g.fillRect(swatch);
            else
                g.drawRect(swatch, stroke::toggle);
            kx += sc::toggleBox + sc::toggleGap;
            text(g, who, labelFont(), colour::label, {kx, y, content.getWidth(), lineH},
                 juce::Justification::centredLeft);
            kx += textWidth(labelFont(), who) + sc::toggleGroupGap;
        }
        y += lineH;
    }

    //  one line only when the picture cannot say it: the host's bus is too narrow, or the other output has the
    //  same channels and would stop
    juce::String status;
    const Placement p = placeOutput(d, output == OutputPage::Headphones ? Output::Headphones : Output::Speakers, bus);
    const juce::String otherName = output == OutputPage::Headphones ? "speakers" : "headphones";
    const bool collide = p.collide;
    if (!mine.empty() && !p.fits)
        status = juce::String(p.needs) + " channels needed; the host gives " + juce::String(bus);
    else if (p.otherOn && p.collide && !mine.empty())
        status = playing   ? "on the same channels as the " + otherName + ": both are heard there"
                 : p.moves ? "playing would move it to " + juce::String(p.movedTo) +
                                 juce::String::fromUTF8("\xe2\x80\x93") + juce::String(p.movedTo + p.span - 1)
                           : "playing would stop " + otherName + ": the same channels";
    if (!status.isEmpty()) {
        y += ctl::segmentGap;
        const float lineH = noteLineHeight();
        text(g, status, labelFont(), !p.fits || (playing && collide) ? colour::text : colour::label,
             {content.getX(), y, content.getWidth(), lineH}, juce::Justification::centredLeft);
        y += lineH;
    }
    return y + static_cast<float>(space::groupGap);
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
    const float lineH = noteLineHeight();
    const float buttonH = ctl::buttonHeight;

    //  what the head is: the model, the KU100 as measured, or a SOFA file fitted on load
    const int head = std::clamp(d.hrtf, 0, 2);
    const juce::Rectangle<float> row{content.getX(), y, content.getWidth(), ctl::segmentHeight};
    paintSegments(g, row, {"model", "ku100", "sofa file"}, head, mine, [this](int which) {
        if (which == 2 && state_.decoder().hrtfFile.empty()) {
            chooseSofaFile();  // no file yet: the chooser, and the state follows the file
            return;
        }
        model().applyEdit("hrtf", [which](PluginState& s) { s.decoder.hrtf = which; });
    });
    y = row.getBottom() + ctl::sectionGap;
    //  the two sliders make the model the listener's; a measured head keeps its ears and scales its time
    //  difference with the head
    paintStateTile(
        g, tileCell(content, y, 0), "head", juce::String(d.headSize, 2) + " " + juce::String::fromUTF8("\xc3\x97"),
        d.headSize, 0.75, 1.3, 0.01, "decoder.head_size", mine,
        [](DecoderState& s, double v) { s.headSize = static_cast<float>(v); },
        [](DecoderState& s) { s.headSize = 1.0f; });
    paintStateTile(
        g, tileCell(content, y, 1), "ears", juce::String(d.earShift, 2) + " erb", d.earShift, -2.0, 2.0, 0.01,
        "decoder.ear_shift", mine && head == 0, [](DecoderState& s, double v) { s.earShift = static_cast<float>(v); },
        [](DecoderState& s) { s.earShift = 0.0f; });
    y = tilesBottom(y, 2) + ctl::sectionGap;
    juce::String about;
    if (sofaError_.isNotEmpty())
        about = "not read: " + sofaError_;
    else if (head == 0)
        about = "learned from measured heads; the sliders make it yours";
    else if (head == 1)
        about = "as measured; head scales its timing, the ears rest";
    else {
        const juce::String name = juce::File(d.hrtfFile).getFileName();
        const juce::String note = state_.processor.sofaNote();
        about = (name.isEmpty() ? juce::String("a sofa file") : name) + " as measured; head scales its timing" +
                (note.isEmpty() ? juce::String() : " " + kDot + " " + note);
    }
    if (head == 2) {
        y = paintButtonNote(
                g, content, y, juce::String::fromUTF8("load\xe2\x80\xa6"), mine, [this] { chooseSofaFile(); }, about) +
            static_cast<float>(space::groupGap);
    } else {
        text(g, about, labelFont(), colour::label, {content.getX(), y, content.getWidth(), lineH},
             juce::Justification::centredLeft);
        y += lineH + static_cast<float>(space::groupGap);
    }

    y = paintChannels(g, content, y, OutputPage::Headphones);

    //  options: the layout heard on headphones, and the correction when it exists
    y = paintGroupTitle(g, content, y, "options");
    const juce::Rectangle<float> through{content.getX(), y, content.getWidth(), lineH};
    const bool hasLayout = !d.layout.empty();
    const auto toggle =
        paintSceneToggle(g, through, content.getX() + sceneToggleWidth("through the layout") + 2.0f * sc::toggleBox,
                         "through the layout", d.throughLayout, mine && hasLayout);
    if (mine && hasLayout)
        addClickArea(toggle.hit, [this] {
            model().applyEdit("through the layout",
                              [](PluginState& s) { s.decoder.throughLayout = !s.decoder.throughLayout; });
        });
    text(g,
         hasLayout ? "the speakers page's " + juce::String(d.layoutName)
                   : juce::String("no layout on the speakers page"),
         labelFont(), colour::label,
         {toggle.hit.getRight() + static_cast<float>(space::grid), y,
          content.getRight() - toggle.hit.getRight() - static_cast<float>(space::grid), lineH},
         juce::Justification::centredLeft);
    y += lineH + ctl::segmentGap;
    y += static_cast<float>(space::groupGap);

    //  the deep part, a tab of its own
    y = paintButtonNote(
        g, content, y, juce::String::fromUTF8("personalise\xe2\x80\xa6"), true,
        [this] { state_.openTemp(TempTab::Personalise); }, "the head's sliders against a real loudspeaker");
    noteContentBottom(y + static_cast<float>(space::grid));
}

void DecoderPanel::chooseSofaFile() {
    chooser_ = std::make_unique<juce::FileChooser>("a measured head (SOFA)", juce::File(), "*.sofa");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this](const juce::FileChooser& fc) {
                              const juce::File file = fc.getResult();
                              if (!file.existsAsFile()) return;
                              juce::String error;
                              sofaError_ = state_.processor.loadSofaFile(file, &error) ? juce::String() : error;
                              repaint();
                          });
}

void DecoderPanel::chooseLayoutFile() {
    chooser_ = std::make_unique<juce::FileChooser>(
        "a loudspeaker layout (IEM AllRADecoder JSON, AmbDec), or a decoder as filters (SOFA FIR-E)", juce::File(),
        "*.json;*.ambdec;*.sofa");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this](const juce::FileChooser& fc) {
                              const juce::File file = fc.getResult();
                              if (!file.existsAsFile()) return;
                              layoutError_.clear();
                              if (file.hasFileExtension("sofa")) {
                                  juce::String error;
                                  if (!state_.processor.loadMatrixFile(file, &error)) layoutError_ = error;
                                  shownKind_ = -1;
                                  repaint();
                                  return;
                              }
                              Layout layout;
                              std::string error;
                              if (!parseLayoutFile(file.loadFileAsString().toStdString(), layout, &error)) {
                                  layoutError_ = juce::String(error);
                                  repaint();
                                  return;
                              }
                              const std::string name = file.getFileNameWithoutExtension().toStdString();
                              shownKind_ = -1;
                              model().applyEdit("load layout", [layout, name](PluginState& s) {
                                  s.decoder.layout = layout.speakers;
                                  s.decoder.layoutName = name;
                                  s.decoder.layoutTyped = false;
                                  s.decoder.matrixFile.clear();
                                  s.decoder.matrixOutputs = 0;
                              });
                          });
}

void DecoderPanel::paintSpeakers(juce::Graphics& g, juce::Rectangle<float> content) {
    const DecoderState& d = state_.decoder();
    const bool mine = !model().showingAnother();
    float y = content.getY();
    const float lineH = noteLineHeight();
    const float buttonH = ctl::buttonHeight;
    const auto line = [&](const juce::String& what, juce::Colour ink) {
        text(g, what, labelFont(), ink, {content.getX(), y, content.getWidth(), lineH},
             juce::Justification::centredLeft);
        y += lineH;
    };

    //  what the loudspeakers are: one of the ITU's, a file, or typed -- typed in, or a loaded one edited
    const bool matrix = !d.matrixFile.empty();
    const bool typedLayout = !matrix && d.layoutTyped;
    const bool ituLayout = !matrix && !typedLayout && !d.layout.empty() && bs2051Layout(d.layoutName) != nullptr;
    const bool fileLayout = matrix || (!d.layout.empty() && !ituLayout && !typedLayout);
    const int kind = shownKind_ >= 0 ? shownKind_ : typedLayout ? 2 : fileLayout ? 1 : 0;
    const juce::Rectangle<float> row{content.getX(), y, content.getWidth(), ctl::segmentHeight};
    paintSegments(g, row, {"itu", "file", "typed"}, kind, mine, [this](int which) {
        shownKind_ = which;
        repaint();
    });
    y = row.getBottom() + ctl::segmentGap;
    const int real = realLoudspeakers(d.layout);
    if (kind == 0) {
        //  the ten systems, the loaded one black
        std::vector<juce::String> names;
        int selected = -1;
        for (std::string_view n : bs2051Names()) {
            if (ituLayout && n == d.layoutName) selected = static_cast<int>(names.size());
            names.push_back(juce::String(std::string(n)));
        }
        y = paintPicker(
                g, content.getX(), y, content.getWidth(), names, selected, mine,
                [this](int which) {
                    const std::string name(bs2051Names()[static_cast<std::size_t>(which)]);
                    const Layout* l = bs2051Layout(name);
                    if (l == nullptr) return;
                    const std::vector<Loudspeaker> speakers = l->speakers;
                    model().applyEdit("layout", [speakers, name](PluginState& s) {
                        s.decoder.layout = speakers;
                        s.decoder.layoutName = name;
                        s.decoder.layoutTyped = false;
                        s.decoder.matrixFile.clear();
                        s.decoder.matrixOutputs = 0;
                    });
                    shownKind_ = -1;
                },
                {}, -1, 5)
                .bottom +
            ctl::segmentGap;
    } else if (kind == 1) {
        //  the file: what is loaded, and what can be
        if (matrix) {
            const juce::String note = state_.processor.matrixNote();
            line(juce::String(d.layoutName) + " " + kDot + " filter matrix" +
                     (note.isEmpty() ? juce::String() : ", " + note),
                 colour::text);
            const juce::Rectangle<float> norm{content.getX(), y, content.getWidth(), ctl::segmentHeight};
            paintSegments(g, norm, {"input as the file says", "sn3d", "n3d"}, std::clamp(d.matrixNormalization, 0, 2),
                          mine, [this](int which) {
                              model().applyEdit("matrix input",
                                                [which](PluginState& s) { s.decoder.matrixNormalization = which; });
                          });
            y = norm.getBottom() + ctl::segmentGap;
            line("what the filters expect on their input; converted as n3d, unconfirmed by the maker", colour::label);
        } else if (fileLayout) {
            line(juce::String(d.layoutName) + " " + kDot + " " + juce::String(real) + " loudspeakers", colour::text);
        } else {
            line("no file loaded", colour::label);
        }
        y = paintButtonNote(
                g, content, y, juce::String::fromUTF8("load\xe2\x80\xa6"), mine, [this] { chooseLayoutFile(); },
                "iem allradecoder json " + kDot + " ambdec " + kDot + " sofa fir-e filter matrix") +
            ctl::segmentGap;
    } else {
        //  typed: the table is the source. Any loaded layout becomes typed the moment it is edited there; a new one
        //  starts empty.
        if (typedLayout)
            line(juce::String(d.layoutName) + " " + kDot + " " + juce::String(real) + " loudspeakers, typed",
                 colour::text);
        else
            line("nothing typed yet: edit the loaded layout in the table, or start empty", colour::label);
        const juce::Rectangle<float> fresh{content.getX(), y, framedButtonWidth("start empty"), buttonH};
        paintButton(g, fresh, "start empty", mine, [this] {
            model().applyEdit("new layout", [](PluginState& s) {
                s.decoder.layout.clear();
                s.decoder.layoutName = "typed";
                s.decoder.layoutTyped = true;
                s.decoder.matrixFile.clear();
                s.decoder.matrixOutputs = 0;
            });
            shownKind_ = -1;
            state_.openTemp(TempTab::Loudspeakers);
        });
        text(g, "then add loudspeakers in the table", labelFont(), colour::label,
             {fresh.getRight() + static_cast<float>(space::grid), y, content.getWidth(), buttonH},
             juce::Justification::centredLeft);
        y += buttonH + ctl::segmentGap;
    }
    const juce::String error = layoutError_.isNotEmpty() ? layoutError_ : state_.processor.designError();
    if (!error.isEmpty()) line("refused: " + error, colour::text);
    y += static_cast<float>(space::groupGap);

    const bool pairLayout = !matrix && realLoudspeakers(d.layout) == 2;

    y = paintChannels(g, content, y, OutputPage::Speakers);

    //  options
    y = paintGroupTitle(g, content, y, "options");
    if (pairLayout) {
        //  two loudspeakers: the fold, or the slice the head points at -- under the channels, so the layout's own
        //  section keeps its height whichever layout is chosen
        const juce::Rectangle<float> pair{content.getX(), y, content.getWidth(), ctl::segmentHeight};
        paintSegments(g, pair, {"fold", "slice"}, d.pairSlice ? 1 : 0, mine, [this](int which) {
            model().applyEdit("pair", [which](PluginState& s) { s.decoder.pairSlice = which == 1; });
        });
        y = pair.getBottom() + ctl::segmentGap;
        line(d.pairSlice ? "only what the head points at plays, between the loudspeakers"
                         : "the whole field on the pair, as a stereo listener hears the mix",
             colour::label);
        y += ctl::segmentGap;
    }
    if (matrix) {
        line("the file's decoder as it is: no weighting, no distance compensation", colour::label);
    } else {
        if (pairLayout) {
            line("microphones take no weighting", colour::label);
        } else {
            const juce::Rectangle<float> seg{content.getX(), y, content.getWidth(), ctl::segmentHeight};
            paintSegments(g, seg, {"one listener", "audience"}, d.weighting == 1 ? 1 : 0, mine, [this](int which) {
                model().applyEdit("weighting", [which](PluginState& s) { s.decoder.weighting = which; });
            });
            y = seg.getBottom() + ctl::segmentGap;
        }
        const juce::Rectangle<float> distanceRow{content.getX(), y, content.getWidth(), lineH};
        const auto distance = paintSceneToggle(
            g, distanceRow, content.getX() + sceneToggleWidth("distance compensation") + 2.0f * sc::toggleBox,
            "distance compensation", d.distanceCompensation, mine);
        if (mine)
            addClickArea(distance.hit, [this] {
                model().applyEdit("distance compensation", [](PluginState& s) {
                    s.decoder.distanceCompensation = !s.decoder.distanceCompensation;
                });
            });
        y += lineH + ctl::segmentGap;
    }
    y += static_cast<float>(space::groupGap);

    //  the table, a tab of its own
    const int order = state_.processor.engineOrder();
    y = paintButtonNote(
        g, content, y, juce::String::fromUTF8("loudspeakers\xe2\x80\xa6"), !d.layout.empty() || matrix,
        [this] { state_.openTemp(TempTab::Loudspeakers); },
        matrix ? juce::String("the outputs, one row each")
               : "order " + juce::String(std::max(order, 0)) + " " + kDot + " the hull is on the scene");
    noteContentBottom(y + static_cast<float>(space::grid));
}

void DecoderPanel::paintLoudspeakers(juce::Graphics& g, juce::Rectangle<float> content) {
    const DecoderState& d = state_.decoder();
    float y = content.getY();
    const float rowH = static_cast<float>(metrics::matrixRow);
    const bool mine = !model().showingAnother();
    const int testing = state_.processor.testing();
    const float testW = framedButtonWidth("test");
    //  a burst on that channel alone, so the loudspeaker and its channel are checked by ear; black while it plays
    const auto testButton = [&](int busChannel, float top, float right) {
        const juce::Rectangle<float> button{right - testW, top + (rowH - ctl::buttonHeight) / 2.0f, testW,
                                            ctl::buttonHeight};
        const bool playing = busChannel == testing;
        paintButton(
            g, button, "test", mine,
            [this, busChannel, playing] { state_.processor.testChannel(playing ? 0 : busChannel); }, playing);
    };
    if (!d.matrixFile.empty()) {
        const int first = std::max(1, d.speakerFirstChannel);
        text(g,
             juce::String(d.matrixOutputs) + " outputs on channels " + juce::String(first) + " to " +
                 juce::String(first + d.matrixOutputs - 1) + "; the file carries no positions",
             labelFont(), colour::label, {content.getX(), y, content.getWidth(), rowH},
             juce::Justification::centredLeft);
        y += rowH;
        for (int o = 0; o < d.matrixOutputs; ++o) {
            text(g, "output " + juce::String(o + 1) + " " + kDot + " channel " + juce::String(first + o),
                 font(type::matrix), colour::text, {content.getX(), y, content.getWidth() - testW, rowH},
                 juce::Justification::centredLeft);
            testButton(first + o, y, content.getRight());
            y += rowH;
            ruleH(g, content.getX(), content.getRight(), y, colour::rowDivider);
        }
    } else {
        //  The loudspeakers, one row each. Every number is dragged like a tile's: azimuth, elevation and distance
        //  are the loudspeaker's, the channel its feed; the box at the left makes it imaginary, the cross at the
        //  right removes it. Any edit makes the layout a typed one. Gain and delay are what distance
        //  compensation gives it, read only.
        const float boxW = sc::toggleBox + static_cast<float>(space::grid) / 2.0f;
        const auto& colW = ctl::tableColumns;
        const char* heads[6] = {"ch", "az", "el", "dist m", "gain db", "delay ms"};
        const float crossW = ctl::closeGlyph + static_cast<float>(space::grid) / 2.0f;
        const float tableRight = content.getRight() - crossW;
        float cx = content.getX() + boxW;
        text(g, "imag", labelFont(), colour::label, {content.getX(), y, boxW + colW[0], rowH},
             juce::Justification::centredLeft);
        for (int c = 1; c < 6; ++c) {
            cx += colW[c - 1];
            text(g, heads[c], labelFont(), colour::label, {cx, y, colW[c], rowH}, juce::Justification::centredRight);
        }
        y += rowH;
        ruleH(g, content.getX(), content.getRight(), y, colour::text);
        y += 2.0f;
        double farthest = 0.0;
        for (const Loudspeaker& s : d.layout)
            if (!s.imaginary) farthest = std::max(farthest, s.radiusM);
        const int shift = std::max(1, d.speakerFirstChannel) - 1;
        //  one drag per cell, as an undo step named for what it moves
        const auto cellDrag = [&](juce::Rectangle<float> area, const char* what, std::size_t i, double from,
                                  double span, double step, double low, double high, bool wraps,
                                  std::function<void(Loudspeaker&, double)> put, double resetTo) {
            if (!mine) return;
            addStateDrag(
                area, what, "decoder.layout." + std::to_string(i) + "." + what, from, span, step,
                [i, put](PluginState& s, double v) {
                    if (i >= s.decoder.layout.size()) return;
                    put(s.decoder.layout[i], v);
                    s.decoder.layoutTyped = true;
                },
                [i, put, resetTo](PluginState& s) {
                    if (i >= s.decoder.layout.size()) return;
                    put(s.decoder.layout[i], resetTo);
                    s.decoder.layoutTyped = true;
                },
                {}, low, high, wraps);
        };
        for (std::size_t i = 0; i < d.layout.size(); ++i) {
            const Loudspeaker& s = d.layout[i];
            const juce::Colour ink = s.imaginary ? colour::label : colour::text;
            const double gainDb = !d.distanceCompensation || s.imaginary || farthest <= 0.0
                                      ? 0.0
                                      : 20.0 * std::log10(s.radiusM / farthest);
            const double delayMs =
                !d.distanceCompensation || s.imaginary ? 0.0 : (farthest - s.radiusM) / 343.0 * 1000.0;
            //  imaginary: in the hull, never fed
            const juce::Rectangle<float> boxRow{content.getX(), y, boxW, rowH};
            const auto imag = paintSceneToggle(g, boxRow, content.getX() + sc::toggleBox, "", s.imaginary, mine);
            if (mine)
                addClickArea(imag.hit, [this, i] {
                    model().applyEdit("imaginary", [i](PluginState& st) {
                        if (i >= st.decoder.layout.size()) return;
                        st.decoder.layout[i].imaginary = !st.decoder.layout[i].imaginary;
                        st.decoder.layoutTyped = true;
                    });
                });
            const juce::String cells[6] = {
                s.imaginary ? juce::String::fromUTF8("\xe2\x80\x93") : juce::String(s.channel + shift),
                juce::String(s.azimuthDeg, 1),
                juce::String(s.elevationDeg, 1),
                juce::String(s.radiusM, 2),
                s.imaginary ? juce::String::fromUTF8("\xe2\x80\x93") : juce::String(gainDb, 1),
                s.imaginary ? juce::String::fromUTF8("\xe2\x80\x93") : juce::String(delayMs, 2)};
            cx = content.getX() + boxW;
            for (int c = 0; c < 6; ++c) {
                const juce::Rectangle<float> cell{cx, y, colW[c], rowH};
                const bool dragged = c <= 3 && !(c == 0 && s.imaginary);
                if (dragged) {
                    //  a value is a box, as the matrix's cell is
                    g.setColour(colour::panel);
                    g.fillRect(cell.reduced(1.0f, (rowH - static_cast<float>(metrics::matrixCell)) / 2.0f));
                }
                text(g, cells[c], font(type::matrix), ink, cell.reduced(static_cast<float>(space::grid) / 2.0f, 0.0f),
                     c == 0 ? juce::Justification::centredLeft : juce::Justification::centredRight);
                cx += colW[c];
                switch (c) {
                    case 0:
                        if (!s.imaginary)
                            cellDrag(
                                cell, "channel", i, s.channel, 32.0, 1.0, 1.0, 128.0, false,
                                [](Loudspeaker& l, double v) { l.channel = static_cast<int>(std::lround(v)); },
                                static_cast<double>(i + 1));
                        break;
                    case 1:
                        cellDrag(
                            cell, "azimuth", i, s.azimuthDeg, 360.0, 1.0, -180.0, 180.0, true,
                            [](Loudspeaker& l, double v) { l.azimuthDeg = v; }, 0.0);
                        break;
                    case 2:
                        cellDrag(
                            cell, "elevation", i, s.elevationDeg, 180.0, 1.0, -90.0, 90.0, false,
                            [](Loudspeaker& l, double v) { l.elevationDeg = v; }, 0.0);
                        break;
                    case 3:
                        cellDrag(
                            cell, "distance", i, s.radiusM, 10.0, 0.05, 0.2, 50.0, false,
                            [](Loudspeaker& l, double v) { l.radiusM = v; }, farthest > 0.0 ? farthest : 2.0);
                        break;
                    default: break;
                }
            }
            if (!s.imaginary) testButton(s.channel + shift, y, tableRight);
            //  the cross removes the row
            const juce::Rectangle<float> crossBox{tableRight + static_cast<float>(space::grid) / 2.0f,
                                                  y + (rowH - ctl::closeGlyph) / 2.0f, ctl::closeGlyph,
                                                  ctl::closeGlyph};
            const juce::Rectangle<float> crossHit = drawCloseCross(g, crossBox);
            if (mine)
                addClickArea(crossHit, [this, i] {
                    model().applyEdit("remove loudspeaker", [i](PluginState& st) {
                        if (i >= st.decoder.layout.size()) return;
                        st.decoder.layout.erase(st.decoder.layout.begin() + static_cast<std::ptrdiff_t>(i));
                        st.decoder.layoutTyped = true;
                    });
                });
            y += rowH;
            ruleH(g, content.getX(), content.getRight(), y, colour::rowDivider);
        }
        if (d.layout.empty()) {
            text(g, "no loudspeakers yet", labelFont(), colour::label, {content.getX(), y, content.getWidth(), rowH},
                 juce::Justification::centredLeft);
            y += rowH;
        }
        y += ctl::segmentGap;
        //  one more, ahead at the farthest distance on the next free channel
        const juce::Rectangle<float> add{content.getX(), y, framedButtonWidth("add a loudspeaker"), ctl::buttonHeight};
        paintButton(g, add, "add a loudspeaker", mine, [this] {
            model().applyEdit("add loudspeaker", [](PluginState& st) {
                Loudspeaker l;
                int top = 0;
                double far = 0.0;
                for (const Loudspeaker& s : st.decoder.layout) {
                    if (!s.imaginary) top = std::max(top, s.channel);
                    far = std::max(far, s.radiusM);
                }
                l.channel = top + 1;
                l.radiusM = far > 0.0 ? far : 2.0;
                st.decoder.layout.push_back(l);
                st.decoder.layoutTyped = true;
                if (st.decoder.layoutName.empty()) st.decoder.layoutName = "typed";
            });
        });
        text(g, "ahead, at the farthest distance, on the next free channel", labelFont(), colour::label,
             {add.getRight() + static_cast<float>(space::grid), y, content.getWidth(), ctl::buttonHeight},
             juce::Justification::centredLeft);
        y += ctl::buttonHeight;
    }
    noteContentBottom(y + static_cast<float>(space::grid));
}

void DecoderPanel::paintTracking(juce::Graphics& g, juce::Rectangle<float> content) {
    const DecoderState& d = state_.decoder();
    const bool mine = !model().showingAnother();
    float y = content.getY();
    const float lineH = noteLineHeight();
    const float buttonH = ctl::buttonHeight;

    //  the chosen source's own setup first; the source itself is chosen in the orientation strip
    if (d.trackerSource == 1) {
        paintStateTile(
            g, tileCell(content, y, 0), "port", juce::String(d.oscPort), d.oscPort, 1024, 65535, 1.0,
            "decoder.osc_port", mine, [](DecoderState& s, double v) { s.oscPort = static_cast<int>(std::lround(v)); },
            [](DecoderState& s) { s.oscPort = 9000; });
        y = tilesBottom(y, 1) + static_cast<float>(space::grid) / 2.0f;
        const juce::Rectangle<float> row{content.getX(), y, content.getWidth(), lineH};
        const auto network =
            paintSceneToggle(g, row, content.getX() + sceneToggleWidth("open to the network") + 2.0f * sc::toggleBox,
                             "open to the network", d.oscNetwork, mine);
        if (mine)
            addClickArea(network.hit, [this] {
                model().applyEdit("osc network", [](PluginState& s) { s.decoder.oscNetwork = !s.decoder.oscNetwork; });
            });
        y += lineH + ctl::segmentGap;
        y = paintLearn(g, content, y);
    } else if (d.trackerSource == 2) {
        const juce::String word = state_.processor.helperStatus();
        text(
            g,
            state_.processor.helperAvailable()
                ? (word.isEmpty() ? juce::String("the helper is starting; it asks once for the motion permission")
                                  : word)
                : juce::String(HelperLink::available() ? "the helper is not in this bundle"
                                                       : "airpods need macOS: the helper that reads them is a mac app"),
            labelFont(), colour::label, {content.getX(), y, content.getWidth(), lineH},
            juce::Justification::centredLeft);
        y += lineH + ctl::segmentGap;
        if (state_.processor.helperAvailable()) {
            //  look and nod: how the AirPods sit on the head, since earbuds sit as they sit. One button pressed
            //  twice: the second press comes while looking down, where nobody sees a second button.
            const bool levelDone = state_.processor.levelCaptured();
            const juce::String lookLabel = "calibrate: look ahead, level, press",
                               nodLabel = "now look down, press again";
            const juce::Rectangle<float> button{
                content.getX(), y, std::max(framedButtonWidth(lookLabel), framedButtonWidth(nodLabel)), buttonH};
            paintButton(g, button, levelDone ? nodLabel : lookLabel, mine, [this, levelDone] {
                if (!levelDone)
                    calibrationNote_ = state_.processor.captureLevel() ? "level kept: look down, press again"
                                                                       : "nothing from the airpods yet";
                else
                    calibrationNote_ = state_.processor.captureNod()
                                           ? "calibrated and recentred"
                                           : "that was no nod: look ahead and press, look down and press again";
                repaint();
            });
            const juce::Rectangle<float> forget{button.getRight() + static_cast<float>(space::grid) / 2.0f, y,
                                                framedButtonWidth("forget"), buttonH};
            paintButton(g, forget, "forget", mine && d.airpodsCalibrated, [this] {
                state_.processor.forgetCalibration();
                calibrationNote_ = "back to apple's frame";
            });
            y += buttonH + ctl::segmentGap;
            const juce::String standing = d.airpodsCalibrated
                                              ? juce::String("calibrated: look and nod found how they sit")
                                              : juce::String("not calibrated: the airpods' own frame, worn upright");
            text(g, calibrationNote_.isEmpty() ? standing : calibrationNote_, labelFont(), colour::label,
                 {content.getX(), y, content.getWidth(), lineH}, juce::Justification::centredLeft);
            y += lineH;
        }
    } else if (d.trackerSource == 3) {
        y = paintPhone(g, content, y);
    } else {
        text(g,
             HelperLink::available()
                 ? "no tracker: the strip's sliders and rates turn the scene. choose osc, airpods or phone there"
                 : "no tracker: the strip's sliders and rates turn the scene. choose osc there; airpods and the phone "
                   "need macOS",
             labelFont(), colour::label, {content.getX(), y, content.getWidth(), lineH},
             juce::Justification::centredLeft);
        y += lineH;
    }
    noteContentBottom(y + static_cast<float>(space::grid));
}

float DecoderPanel::paintLearn(juce::Graphics& g, juce::Rectangle<float> content, float y) {
    const DecoderState& d = state_.decoder();
    const bool mine = !model().showingAnother();
    const float lineH = noteLineHeight();
    const auto line = [&](const juce::String& what, juce::Colour ink) {
        text(g, what, labelFont(), ink, {content.getX(), y, content.getWidth(), lineH},
             juce::Justification::centredLeft);
        y += lineH;
    };
    const bool learned = d.oscForm != 0 && !d.oscAddresses[0].empty();
    const bool split = learned && !d.oscAddresses[1].empty() && !d.oscAddresses[2].empty();
    if (!learned)
        line("format: by address " + kDot + " adm-osc, sparta and iem; anything else is learned from the list below",
             colour::label);
    else if (split)
        line("format: learned " + kDot + " yaw " + d.oscAddresses[0] + ", pitch " + d.oscAddresses[1] + ", roll " +
                 d.oscAddresses[2],
             colour::text);
    else
        line("format: learned " + kDot + " " + d.oscAddresses[0], colour::text);
    if (learned) {
        //  what the learned message carries; for split addresses a quaternion makes no sense and reads as degrees
        const juce::Rectangle<float> forms{content.getX(), y, content.getWidth(), ctl::segmentHeight};
        paintSegments(g, forms, {"quat w first", "quat w last", "degrees", "deg, pitch down", "radians"},
                      std::clamp(d.oscForm, 1, 5) - 1, mine, [this](int which) {
                          model().applyEdit("osc form", [which](PluginState& s) { s.decoder.oscForm = which + 1; });
                      });
        y = forms.getBottom() + ctl::segmentGap;
        const float buttonH = ctl::buttonHeight;
        const juce::Rectangle<float> forget{content.getX(), y, framedButtonWidth("forget"), buttonH};
        paintButton(g, forget, "forget", mine, [this] {
            model().applyEdit("forget learned osc", [](PluginState& s) {
                s.decoder.oscAddresses = {};
                s.decoder.oscForm = 0;
            });
        });
        text(g, "back to the known addresses", labelFont(), colour::label,
             {forget.getRight() + static_cast<float>(space::grid), y, content.getWidth(), buttonH},
             juce::Justification::centredLeft);
        y += buttonH + ctl::segmentGap;
    }
    const std::vector<BambiDecoderProcessor::SeenAddress> seen = state_.processor.seenAddresses();
    if (seen.empty()) {
        line("nothing on port " + juce::String(d.oscPort) + " yet", colour::label);
        return y;
    }
    line("on the port now " + kDot + " take a message whole, or one number as yaw, pitch or roll:", colour::label);
    const char* slots[4] = {"message", "yaw", "pitch", "roll"};
    const float slotW = ctl::learnSlotWidth;
    int shown = 0;
    for (const BambiDecoderProcessor::SeenAddress& s : seen) {
        if (shown++ >= 6) break;
        const float wordsX = content.getRight() - 4.0f * slotW;
        text(g, s.address, font(type::matrix), colour::text, {content.getX(), y, wordsX - content.getX() - 4.0f, lineH},
             juce::Justification::centredLeft);
        for (int k = 0; k < 4; ++k) {
            const juce::Rectangle<float> word{wordsX + static_cast<float>(k) * slotW, y, slotW, lineH};
            const bool chosen =
                k == 0 ? (!split && learned && d.oscAddresses[0] == s.address.toStdString())
                       : (split && d.oscAddresses[static_cast<std::size_t>(k - 1)] == s.address.toStdString());
            text(g, slots[k], labelFont(), chosen ? colour::text : colour::label, word,
                 juce::Justification::centredRight);
            if (!mine) continue;
            const std::string address = s.address.toStdString();
            const int numbers = s.arguments;
            addClickArea(word, [this, k, address, numbers] {
                model().applyEdit("learn osc", [k, address, numbers](PluginState& st) {
                    DecoderState& dd = st.decoder;
                    if (k == 0) {
                        dd.oscAddresses = {address, "", ""};
                        if (dd.oscForm == 0 || dd.oscForm == 3) dd.oscForm = numbers >= 4 ? 1 : 3;
                    } else {
                        if (dd.oscAddresses[1].empty() && dd.oscAddresses[2].empty() && k != 1)
                            dd.oscAddresses[0].clear();
                        dd.oscAddresses[static_cast<std::size_t>(k - 1)] = address;
                        if (dd.oscForm == 0 || dd.oscForm == 1 || dd.oscForm == 2) dd.oscForm = 3;
                    }
                });
            });
        }
        y += lineH;
        juce::String preview = juce::String(s.arguments) + (s.arguments == 1 ? " number" : " numbers");
        for (int i = 0; i < std::min(s.arguments, 4); ++i)
            preview += (i == 0 ? ": " : "  ") + juce::String(s.values[static_cast<std::size_t>(i)], 2);
        line(preview, colour::label);
    }
    return y;
}

float DecoderPanel::paintPhone(juce::Graphics& g, juce::Rectangle<float> content, float y) {
    const DecoderState& d = state_.decoder();
    const float lineH = noteLineHeight();
    const auto line = [&](const juce::String& what, juce::Colour ink, float x, float width) {
        text(g, what, labelFont(), ink, {x, y, width, lineH}, juce::Justification::centredLeft);
        y += lineH;
    };
    if (!state_.processor.helperAvailable()) {
        line(HelperLink::available() ? "the helper is not in this bundle"
                                     : "the phone's page needs macOS: the helper that serves it is a mac app",
             colour::label, content.getX(), content.getWidth());
        return y;
    }
    const juce::String url = state_.processor.phoneUrl(), status = state_.processor.phoneStatus();
    if (url.isEmpty()) {
        line(status.isEmpty() ? juce::String("the helper is starting") : status, colour::label, content.getX(),
             content.getWidth());
        return y;
    }
    //  the code, modules as squares on the page's own ground, a quiet zone of two modules around it
    const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(url.toRawUTF8(), qrcodegen::QrCode::Ecc::MEDIUM);
    const float module = static_cast<float>(space::grid) / 2.0f, quiet = 2.0f * module;
    const float side = static_cast<float>(qr.getSize()) * module + 2.0f * quiet;
    const juce::Rectangle<float> code{content.getX(), y, side, side};
    g.setColour(colour::ground);
    g.fillRect(code);
    g.setColour(colour::text);
    for (int row = 0; row < qr.getSize(); ++row)
        for (int col = 0; col < qr.getSize(); ++col)
            if (qr.getModule(col, row))
                g.fillRect(code.getX() + quiet + static_cast<float>(col) * module,
                           code.getY() + quiet + static_cast<float>(row) * module, module, module);
    //  beside it: the address, the three steps, and what the helper says
    const float textX = code.getRight() + static_cast<float>(space::grid), textW = content.getRight() - textX;
    const float top = y;
    line(url, colour::text, textX, textW);
    line("1  point the phone's camera at the code and open the page", colour::label, textX, textW);
    line("2  it warns the connection is not private " + kDot + " iPhone: show details, visit this website " + kDot +
             " android: advanced, proceed",
         colour::label, textX, textW);
    line("3  say how the phone is worn, tap start, face the front and press recentre", colour::label, textX, textW);
    line(status, colour::label, textX, textW);
    return std::max(y, top + side);
}

void DecoderPanel::paintPersonalise(juce::Graphics& g, juce::Rectangle<float> content) {
    float y = content.getY();
    text(g,
         "a real loudspeaker and a virtual one alternate; turn and nod; move each slider until the two sit in one "
         "place.",
         labelFont(), colour::label, {content.getX(), y, content.getWidth(), static_cast<float>(space::grid * 5)},
         juce::Justification::topLeft);
    y += static_cast<float>(space::grid * 5);
    noteContentBottom(y + static_cast<float>(space::grid));
}

}  // namespace bambi::ui
