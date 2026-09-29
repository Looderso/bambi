// SPDX-License-Identifier: GPL-3.0-or-later
#include "UI/ParameterTabs.h"

#include <algorithm>
#include <array>
#include <string>

#include "bambi/encode/params.hpp"
#include "bambi/encode/pathparams.hpp"
#include "bambi/math/vec3.hpp"
#include "bambi/path/authoring.hpp"
#include "bambi/path/generator.hpp"
#include "bambi/ui/Draw.h"

namespace bambi::ui {
namespace {
namespace colour = theme::colour;
namespace ctl = theme::controls;

constexpr std::array<const char*, 3> kTabNames{"trajectory", "transform", "source"};
constexpr std::array<bambi::GeneratorType, 5> kShapes{bambi::GeneratorType::Orbit, bambi::GeneratorType::Lissajous,
                                                      bambi::GeneratorType::Wave, bambi::GeneratorType::Arc,
                                                      bambi::GeneratorType::Spiral};
}  // namespace

ParameterTabs::ParameterTabs(ControlState& state, SceneState& scene)
    : ParameterPage(state), state_(state), scene_(scene) {
    setOpaque(true);
}

void ParameterTabs::paint(juce::Graphics& g) {
    clearRegions();
    liveRegion() = {};
    clearNameAreas();
    stepAreas_ = {};

    //  the bar, the rule, the viewport, the remote notice and the temporary source tab: the page's
    const auto frame = paintTabbedFrame(g, {kTabNames[0], kTabNames[1], kTabNames[2]});
    if (frame.content.isEmpty()) return;  // another instance, not yet answered: the notice has the panel

    const ContentClip clip(*this, g);  // the tab bar stays put; what it governs scrolls under it
    switch (state_.tab) {
        case 0: paintTrajectory(g, frame.content); break;
        case 1: paintTransform(g, frame.content); break;
        case 2: paintSource(g, frame.content); break;
        default: paintSourceSettings(g, frame.content); break;
    }
}

/*  Every tile's live mark is what the engine published for it -- except these four, whose truth is
    not a parameter's value: the placement's angles carry the turn their rates have added, and displace the
    distance the motion has travelled. An angle wraps, so a yaw the clock has carried past 180 degrees
    reappears at the other end of the bar: the jump is the wrap being visible. */
bool ParameterTabs::liveValue(bambi::ParamId id, double& out) const {
    const auto* shown = scene_.find(scene_.selected);
    if (shown == nullptr) return ParameterPage::liveValue(id, out);
    const auto& x = shown->transform;
    switch (id) {
        case bambi::EncoderParam::TransformYaw: out = x.yawRad * bambi::kRad2Deg; return true;
        case bambi::EncoderParam::TransformPitch: out = x.pitchRad * bambi::kRad2Deg; return true;
        case bambi::EncoderParam::TransformRoll: out = x.rollRad * bambi::kRad2Deg; return true;
        case bambi::EncoderParam::MotionDisplace:
            /*  Where the source is, as the displace that would put it there with nothing travelled.
                Displace is one lap, 0 to 1, and on every kind of path its value is the place. */
            out = std::clamp(shown->s, 0.0, 1.0);
            return true;
        default: return ParameterPage::liveValue(id, out);  // what the engine published
    }
}

void ParameterTabs::onDefaultRestored(bambi::ParamId id) {
    //  a region angle is every plugin's, and the shared page answers it
    ParameterPage::onDefaultRestored(id);

    /*  The placement's is the encoder's alone -- no effect has a placement -- which is why this much
        is here and not in the shared page. */
    if (id == bambi::EncoderParam::TransformYaw)
        state_.target().zeroTurn(0);
    else if (id == bambi::EncoderParam::TransformPitch)
        state_.target().zeroTurn(1);
    else if (id == bambi::EncoderParam::TransformRoll)
        state_.target().zeroTurn(2);
    else if (id == bambi::EncoderParam::MotionDisplace)
        state_.target().zeroTurn(3);  // and the distance travelled with it
}

void ParameterTabs::addWidthRangeTile(juce::Graphics& g, juce::Rectangle<float> tile) {
    const auto low = static_cast<double>(state_.value(bambi::EncoderParam::RenderWidthMin));
    const auto high = static_cast<double>(state_.value(bambi::EncoderParam::RenderWidthMax));
    TileContent t;
    t.name = "width range";
    t.value = fromCore(bambi::formatNumber(low, 0) + " \xe2\x80\x93 " +
                       bambi::formatParameter(bambi::EncoderParam::RenderWidthMax, high));
    t.fillFrom = normalised(bambi::encodeParams(), bambi::EncoderParam::RenderWidthMin, low);
    t.fillTo = normalised(bambi::encodeParams(), bambi::EncoderParam::RenderWidthMax, high);
    drawTile(g, tile, t);

    //  Two values in one box: the left half is the minimum, the right half the maximum.
    const auto values = tileBox(tile);
    addHostDrag(values.withWidth(values.getWidth() / 2.0f), bambi::EncoderParam::RenderWidthMin, false);
    addHostDrag(values.withTrimmedLeft(values.getWidth() / 2.0f), bambi::EncoderParam::RenderWidthMax, false);
}

void ParameterTabs::paintTransform(juce::Graphics& g, juce::Rectangle<float> content) {
    //  Three rotations, each beside its own rate. Yaw and pitch turn the path about the
    //  world's up and left axes; roll turns it about its own centre.
    const auto y = paintGroup(
        g, content, content.getY(), {},
        {bambi::EncoderParam::TransformYaw, bambi::EncoderParam::TransformYawRate, bambi::EncoderParam::TransformPitch,
         bambi::EncoderParam::TransformPitchRate, bambi::EncoderParam::TransformRoll,
         bambi::EncoderParam::TransformRollRate, bambi::EncoderParam::TransformExtent});
    noteContentBottom(y + scroll());
}

void ParameterTabs::paintSource(juce::Graphics& g, juce::Rectangle<float> content) {
    auto y = content.getY();

    /*  What a stereo input puts on the sphere, first, because it decides what "the source" is: one
        point, a point with its sides spread about it, or two points along the path. A choice
        of three is segments. The spread and the offset are both on screen whatever it says,
        the one not in use greyed and inert, so the group does not change shape with the mode.
        A mono track has no side, and every mode is then `sum`: the choice is drawn greyed and says so,
        rather than offering two modes that would do nothing. Another instance's bus is not known
        here, so its choice is offered whole. */
    //  asked of the bus and not of a diagnostic the audio thread fills: that one is 0 until the first block
    const bool mono = !state_.remote && state_.processor.getMainBusNumInputChannels() < 2;
    const int mode = std::clamp(juce::roundToInt(state_.value(bambi::EncoderParam::InputMode)), 0, 2);
    const juce::Rectangle<float> modes{content.getX(), y, content.getWidth(), ctl::segmentHeight};
    paintSegments(g, modes, {"sum", "mid/side", "stereo"}, mono ? 0 : mode, !mono, [this](int choice) {
        model().changeParameter(bambi::EncoderParam::InputMode, static_cast<float>(choice) / 2.0f);
    });
    noteControl(bambi::EncoderParam::InputMode, modes, !mono);
    y = modes.getBottom() + ctl::segmentGap;
    if (mono) {
        const auto line = theme::type::label * ctl::noticeLineHeight;
        text(g, "a mono track has no side: it is one point", labelFont(), colour::label,
             {content.getX(), y, content.getWidth(), line});
        y += line;
    }
    y += ctl::sectionGap - ctl::segmentGap;
    addParameterTile(g, tileCell(content, y, 0), bambi::EncoderParam::InputSpread, "spread", !mono && mode == 1);
    addParameterTile(g, tileCell(content, y, 1), bambi::EncoderParam::InputOffset, "offset", !mono && mode == 2);
    addParameterTile(g, tileCell(content, y, 2), bambi::EncoderParam::InputTrim, "input trim");
    y = tilesBottom(y, 3) + static_cast<float>(theme::space::groupGap);

    y = paintGroup(g, content, y, "motion",
                   {bambi::EncoderParam::MotionDisplace, bambi::EncoderParam::MotionSpeed,
                    bambi::EncoderParam::MotionDirection, bambi::EncoderParam::MotionMode});
    //  restart or continue -- for the motion, the placement's rates and the region's -- is the
    //  footer's `rates continue`, always on screen
    //  the gain is not here: it is the level column's, always on screen, as an effect's dry and wet are
    paintGroup(g, content, y, "render",
               {bambi::EncoderParam::RenderWidth,
                Cell{[this](juce::Graphics& gg, juce::Rectangle<float> tile) { addWidthRangeTile(gg, tile); }}});
}

void ParameterTabs::paintTrajectory(juce::Graphics& g, juce::Rectangle<float> content) {
    const auto& trajectory = state_.patch.trajectory;
    const bool parametric = trajectory.kind == bambi::TrajectoryKind::Parametric;
    auto y = content.getY();

    //  One row: the five generators, then custom. Picking a generator never writes the chain, so
    //  trying other shapes loses nothing; a chain comes from "convert to custom" alone, and until there is one,
    //  custom is shown but not offered. Coming back to the generator that was left keeps its parameters.
    const bool hasChain = trajectory.nodes.size() >= 2;
    std::vector<juce::String> names;
    for (const auto shape : kShapes) names.push_back(fromCore(bambi::name(shape)));
    names.push_back("custom");
    int selected = static_cast<int>(kShapes.size());
    if (parametric)
        for (std::size_t i = 0; i < kShapes.size(); ++i)
            if (trajectory.generator == kShapes[i]) selected = static_cast<int>(i);
    y = paintPicker(
            g, content.getX(), y, content.getWidth(), names, selected, true,
            [this](int choice) {
                if (choice == static_cast<int>(kShapes.size())) {
                    state_.edit("custom path", [](bambi::PluginState& s) {
                        //  Another window may have taken the chain away between this paint and the click.
                        if (s.trajectory.nodes.size() >= 2) s.trajectory.kind = bambi::TrajectoryKind::Custom;
                    });
                    return;
                }
                const auto shape = kShapes[static_cast<std::size_t>(choice)];
                state_.edit("shape", [shape](bambi::PluginState& s) {
                    s.trajectory.kind = bambi::TrajectoryKind::Parametric;
                    if (s.trajectory.generator == shape) return;  // back from custom: its parameters as they were
                    s.trajectory.generator = shape;
                    bambi::generatorDefaults(shape, s.trajectory.genParams);
                });
            },
            {}, hasChain ? -1 : static_cast<int>(kShapes.size()), 3)
            .bottom;
    y += ctl::segmentGap;

    //  Closed or open is a choice only for a custom path; a shape is whichever it is, so it is not shown there.
    if (!parametric) {
        paintSegments(g, {content.getX(), y, content.getWidth(), ctl::segmentHeight}, {"closed", "open"},
                      trajectory.closed ? 0 : 1, true, [this](int choice) {
                          const bool wantClosed = choice == 0;
                          state_.edit(wantClosed ? "close path" : "open path", [wantClosed](bambi::PluginState& s) {
                              bambi::setClosed(s.trajectory, wantClosed);
                          });
                      });
        y += ctl::segmentHeight + ctl::segmentGap;
    }
    y += ctl::sectionGap - ctl::segmentGap;
    ruleH(g, content.getX(), content.getRight(), y, colour::rule);
    y += ctl::contentTop;

    //  A custom path has no shape to choose: its nodes are edited in the scene, and the selected node's controls
    //  come straight after the row, where they are seen without scrolling.
    if (!parametric) {
        noteContentBottom(paintNode(g, content, y) + scroll());
        return;
    }

    //  the shape's parameters
    const auto info = bambi::generatorParams(trajectory.generator);
    const auto columnWidth = (content.getWidth() - ctl::tileColumnGap) / 2.0f;
    const auto height = tileHeight(theme::type::valueShape);
    for (std::size_t i = 0; i < info.size(); ++i) {
        const auto& p = info[i];
        const auto column = static_cast<float>(i % 2);
        const auto row = static_cast<float>(i / 2);
        const juce::Rectangle<float> tile{content.getX() + column * (columnWidth + ctl::tileColumnGap),
                                          y + row * (height + ctl::tileRowGap), columnWidth, height};
        //  a continuous setting is a host parameter: the page's own tile -- row click, live mark, automation
        if (const auto id = bambi::pathParamId(trajectory.generator, static_cast<int>(i)); id != bambi::kNoParamId) {
            addParameterTile(g, tile, id, fromCore(p.label));
            continue;
        }
        const double value = trajectory.genParams[i];
        TileContent t;
        t.name = fromCore(p.label);
        t.value = fromCore(bambi::formatNumber(value, 0) + (p.unit == "deg" ? "\xc2\xb0" : ""));
        t.valueSize = theme::type::valueShape;
        const bool signedShape = p.min < 0.0 && p.max > 0.0;
        t.fillFrom = signedShape ? (0.0 - p.min) / (p.max - p.min) : 0.0;
        t.tick = signedShape;
        t.fillTo = (value - p.min) / (p.max - p.min);
        drawTile(g, tile, t);

        //  the shared state drag: its range, its step, and whether it is one turn and wraps
        addStateDrag(
            tileBox(tile), "shape parameter", "shape." + std::string(p.name), value, p.max - p.min, p.step,
            [i](bambi::PluginState& s, double v) { s.trajectory.genParams[i] = v; },
            [i](bambi::PluginState& s) {
                std::array<double, bambi::kMaxGenParams> defaults{};
                bambi::generatorDefaults(s.trajectory.generator, defaults);
                s.trajectory.genParams[i] = defaults[i];
            },
            {}, p.min, p.max, p.wraps, {tileBar(tile), p.min, p.max});
    }
    const auto rows = static_cast<float>((info.size() + 1) / 2);
    y += rows * (height + ctl::tileRowGap) - ctl::tileRowGap + ctl::sectionGap;

    const auto line = theme::type::label * ctl::noticeLineHeight;
    //  The one way to a chain: this shape as it is now, converted, and it always replaces the chain there was.
    const bool replaces = trajectory.nodes.size() >= 2;
    paintButton(g, {content.getX(), y, content.getWidth(), ctl::buttonHeight}, "convert to custom", true, [this] {
        state_.edit("convert to custom", [](bambi::PluginState& s) {
            bambi::pathSettingsFrom(s.trajectory, s.params);  // the path as its parameters set it
            bambi::convertToCustom(s.trajectory);
            s.trajectory.kind = bambi::TrajectoryKind::Custom;
        });
        scene_.selectedNode = -1;
    });
    y += ctl::buttonHeight + ctl::segmentGap;
    if (replaces) {
        text(g, "replaces the custom path you have", labelFont(), colour::label,
             {content.getX(), y, content.getWidth(), line});
        y += line;
    }
    noteContentBottom(y + scroll());
}

/*  The node selected in the scene. Where it is and how its handles bend are edited there, on the sphere, and
    never automated or modulated, so they have no numbers here. What is left is what has no gesture. */
float ParameterTabs::paintNode(juce::Graphics& g, juce::Rectangle<float> content, float y) {
    const auto& chain = state_.patch.trajectory;
    const auto line = theme::type::label * ctl::noticeLineHeight;
    const int index = scene_.selectedNode;
    const bool has = index >= 0 && static_cast<std::size_t>(index) < chain.nodes.size();

    y = paintGroupTitle(
        g, content, y,
        has ? "node " + juce::String(index + 1) + " of " + juce::String(chain.nodes.size()) : juce::String("node"));
    if (!has) {
        text(g, "click a node in the scene, or the path to add one", labelFont(), colour::label,
             {content.getX(), y, content.getWidth(), line});
        noteContentBottom(y + line + scroll());
        return y + line;
    }

    const auto at = static_cast<std::size_t>(index);
    const auto& node = chain.nodes[at];

    //  smooth or corner: the same thing alt-dragging a handle does, said in words
    paintSegments(g, {content.getX(), y, content.getWidth(), ctl::segmentHeight}, {"smooth", "corner"},
                  node.smooth ? 0 : 1, true, [this, at](int choice) {
                      state_.edit(choice == 0 ? "smooth node" : "corner node", [at, choice](bambi::PluginState& s) {
                          choice == 0 ? bambi::makeSmooth(s.trajectory, at) : bambi::makeCorner(s.trajectory, at);
                      });
                  });
    y += ctl::segmentHeight + ctl::segmentGap;

    const auto half = (content.getWidth() - ctl::tileColumnGap) / 2.0f;
    paintButton(g, {content.getX(), y, half, ctl::buttonHeight}, "delete", chain.nodes.size() > 2, [this, at] {
        state_.edit("delete node", [at](bambi::PluginState& s) { bambi::deleteNode(s.trajectory, at); });
        scene_.selectedNode = -1;
    });
    //  Opening a closed path cuts the closing segment; make start chooses where that seam falls.
    paintButton(g, {content.getX() + half + ctl::tileColumnGap, y, half, ctl::buttonHeight}, "make start",
                chain.closed && index > 0, [this, at] {
                    state_.edit("make start", [at](bambi::PluginState& s) { bambi::makeStart(s.trajectory, at); });
                    scene_.selectedNode = 0;
                });
    return y + ctl::buttonHeight;
}

}  // namespace bambi::ui
