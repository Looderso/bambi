// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/ui/RegionEditor.h"

#include <algorithm>
#include <cmath>

#include "bambi/math/sh.hpp"
#include "bambi/mod/regionclip.hpp"
#include "bambi/region/projection.hpp"
#include "bambi/region/shape.hpp"
#include "bambi/ui/Draw.h"
#include "bambi/ui/RegionClipboard.h"
#include "bambi/ui/Widgets.h"

namespace bambi::ui {

namespace {
namespace colour = theme::colour;
namespace ctl = theme::controls;

/// Roll shows only where a kind can turn about its own axis: sectors, and dots that are not a pair.
bool rollShows(const RegionShape& shape) {
    return shape.custom || shape.kind == RegionKind::Clouds || shape.kind == RegionKind::Sectors ||
           (shape.kind == RegionKind::Dots && shape.dots != 2);
}

/// The tiles a kind has, beyond softness. "Everywhere" has none and says so.
std::vector<const char*> shapeFields(RegionKind kind) {
    switch (kind) {
        case RegionKind::Spot: return {"size"};
        case RegionKind::Band: return {"band_elevation", "thickness"};
        case RegionKind::Sectors: return {"fill"};
        case RegionKind::Dots: return {"dot_size"};
        case RegionKind::Clouds: return {"coverage", "contrast", "detail", "evolve"};
        default: return {};
    }
}
}  // namespace

namespace {
/*  The pyramid: one cell per harmonic, order n down the rows, degree m across, up to the plugin's
    order -- a row the bus does not play is not shown, and the weights above are kept in state for
    a session that runs higher. A cell is a bar: its weight as a share of the largest, filled from
    the bottom, in ink above zero and in the live colour below it. At the right of each row its
    gain, a bar with a tick at 1 and its number: what plays is the cell times the row's gain, and
    the cells keep what was frozen. Drag a cell for its weight, the gain for its order; double-click
    a cell for its converted weight, a gain for 1. */
float paintPyramid(ParameterPage& page, juce::Graphics& g, juce::Rectangle<float> content, float y,
                   const RegionSlot& slot, const RegionShape& shape,
                   const std::function<std::array<double, kRegionWeights>()>& projected) {
    const int N = std::clamp(page.model().renderedOrder(), 1, kRegionWeightsOrder);
    constexpr float gap = 2.0f, labelWidth = 24.0f, gainBar = 52.0f, gainText = 34.0f;
    constexpr double gainTop = 1.5;  // the bar's full extent; the tick is at 1
    const float gainWidth = gainBar + gainText;
    const float cellsWidth = content.getWidth() - labelWidth - gainWidth - 2.0f * gap;
    const float cell = std::floor(std::min(18.0f, cellsWidth / static_cast<float>(2 * N + 1) - gap));
    const float centre = content.getX() + labelWidth + gap + cellsWidth / 2.0f;
    double largest = 1e-9;  // the tallest cell is full, whatever the weights' scale
    for (int i = 0; i < numChannels(N); ++i)
        largest = std::max(largest, std::abs(shape.weights[static_cast<std::size_t>(i)]));

    for (int n = 0; n <= N; ++n) {
        const float rowY = y + static_cast<float>(n) * (cell + gap);
        text(g, "n" + juce::String(n), labelFont(), colour::label, {content.getX(), rowY, labelWidth, cell},
             juce::Justification::centredLeft);

        const float x0 = centre - static_cast<float>(2 * n + 1) * (cell + gap) / 2.0f;
        for (int m = -n; m <= n; ++m) {
            const int i = n * n + n + m;
            const auto w = shape.weights[static_cast<std::size_t>(i)];
            const juce::Rectangle<float> box{x0 + static_cast<float>(m + n) * (cell + gap), rowY, cell, cell};
            g.setColour(colour::panel);
            g.fillRect(box);
            const float fill = static_cast<float>(std::min(1.0, std::abs(w) / largest)) * (cell - 2.0f);
            if (fill > 0.0f) {
                g.setColour(w >= 0.0 ? colour::valueFill : colour::liveValue);
                g.fillRect(juce::Rectangle<float>{box.getX() + 1.0f, box.getBottom() - 1.0f - fill, cell - 2.0f, fill});
            }
            g.setColour(colour::rule);
            g.drawRect(box, theme::stroke::rule);
            page.addStateDrag(
                box, "custom weight", slot.prefix + ".w" + std::to_string(i), w, 2.0, 0.01,
                [at = slot.index, i](PluginState& s, double v) {
                    s.regions[static_cast<std::size_t>(at)].shape.weights[static_cast<std::size_t>(i)] = v;
                },
                [at = slot.index, i, projected](PluginState& s) {
                    s.regions[static_cast<std::size_t>(at)].shape.weights[static_cast<std::size_t>(i)] =
                        projected()[static_cast<std::size_t>(i)];
                },
                {}, -2.0, 2.0);
        }

        //  the row's gain: a bar with a tick at 1, and its number
        const double gain = shape.gains[static_cast<std::size_t>(n)];
        const float gx = content.getRight() - gainWidth, gy = rowY + (cell - ctl::barHeight) / 2.0f;
        const juce::Rectangle<float> track{gx, gy, gainBar, ctl::barHeight};
        g.setColour(colour::track);
        g.fillRect(track);
        g.setColour(colour::valueFill);
        g.fillRect(track.withWidth(static_cast<float>(std::clamp(gain / gainTop, 0.0, 1.0)) * gainBar));
        g.setColour(colour::barTick);
        g.fillRect(juce::Rectangle<float>{gx + static_cast<float>(1.0 / gainTop) * gainBar, gy - 2.0f, 1.0f,
                                          ctl::barHeight + 4.0f});
        text(g, juce::String(gain, 2), labelFont(), colour::label, {gx + gainBar + 4.0f, rowY, gainText - 4.0f, cell},
             juce::Justification::centredLeft);
        page.addStateDrag(
            {gx, rowY, gainWidth, cell}, "order gain", slot.prefix + ".gain" + std::to_string(n), gain, gainTop, 0.01,
            [at = slot.index, n](PluginState& s, double v) {
                s.regions[static_cast<std::size_t>(at)].shape.gains[static_cast<std::size_t>(n)] = v;
            },
            [at = slot.index, n](PluginState& s) {
                s.regions[static_cast<std::size_t>(at)].shape.gains[static_cast<std::size_t>(n)] = 1.0;
            },
            {}, 0.0, 2.0);
    }
    y += static_cast<float>(N + 1) * (cell + gap) - gap + ctl::sectionGap;

    auto& model = page.model();
    const auto half = (content.getWidth() - ctl::tileColumnGap) / 2.0f;
    const juce::Rectangle<float> normalise{content.getX(), y, half, ctl::buttonHeight};
    const juce::Rectangle<float> reset{content.getX() + half + ctl::tileColumnGap, y, half, ctl::buttonHeight};
    //  the field's peak over the sphere, on a grid fine enough for order 7, scaled to 1 through the cells
    page.paintButton(g, normalise, "normalise peak to 1", true, [&model, slot, at = slot.index] {
        const RegionField field(regionOf(model, slot));
        double peak = 0.0;
        for (int a = 0; a < 48; ++a)
            for (int b = 0; b < 96; ++b) {
                const double z = -1.0 + (a + 0.5) * 2.0 / 48.0, az = (b + 0.5) * 2.0 * kPi / 96.0,
                             s = std::sqrt(1.0 - z * z);
                peak = std::max(peak, std::abs(field.at({s * std::cos(az), s * std::sin(az), z})));
            }
        if (peak <= 0.0) return;
        model.applyEdit("normalise custom", [at, peak](PluginState& s) {
            for (double& w : s.regions[static_cast<std::size_t>(at)].shape.weights) w /= peak;
        });
        model.notifyChanged();
    });
    page.paintButton(g, reset, "reset all", true, [&model, at = slot.index, projected] {
        const auto w = projected();
        model.applyEdit("reset custom", [at, w](PluginState& s) {
            auto& r = s.regions[static_cast<std::size_t>(at)].shape;
            r.weights = w;
            r.gains = RegionShape{}.gains;
        });
        model.notifyChanged();
    });
    return reset.getBottom() + static_cast<float>(theme::space::groupGap);
}

/// The transform sub-tab: where the region points and how it turns, then how it is used.
float paintTransformTab(ParameterPage& page, juce::Graphics& g, juce::Rectangle<float> content, float y,
                        const RegionSlot& slot) {
    auto& model = page.model();
    const auto id = [&](const char* field) {
        return static_cast<ParamId>(model.manifest().byKey(slot.prefix + "." + field));
    };
    const auto shape = model.state().regions[static_cast<std::size_t>(slot.index)].shape;

    //  each angle beside its rate
    const bool none = shape.kind == RegionKind::Everywhere;
    const std::array<const char*, 3> axes{"yaw", "pitch", "roll"};
    const std::array<const char*, 3> rates{"yaw_rate", "pitch_rate", "roll_rate"};
    for (int row = 0; row < 3; ++row) {
        const bool enabled = !none && (row != 2 || rollShows(shape));
        page.addParameterTile(g, tileCell(content, y, row * 2), id(axes[static_cast<std::size_t>(row)]), {}, enabled);
        page.addParameterTile(g, tileCell(content, y, row * 2 + 1), id(rates[static_cast<std::size_t>(row)]), {},
                              enabled);
    }
    //  what the rates do when playback stops and starts is the footer's: `rates continue`
    y = tilesBottom(y, 6) + static_cast<float>(theme::space::groupGap);

    //  ---- use: the use's, not the region's, which is why copy does not carry them ----------------
    y = page.paintGroupTitle(g, content, y, "use");
    const auto side = id("side");
    const juce::Rectangle<float> sides{content.getX(), y, content.getWidth(), ctl::segmentHeight};
    page.paintSegments(g, sides, {"inside", "outside"}, model.valueOf(side) > 0.5f ? 1 : 0, !none,
                       [&model, side](int which) { model.changeParameter(side, which > 0 ? 1.0f : 0.0f); });
    page.noteControl(side, sides, !none);
    y = sides.getBottom() + ctl::sectionGap;

    const auto amount =
        static_cast<ParamId>(model.manifest().byKey("mod.amount.region" + std::to_string(slot.index + 1)));
    page.addParameterTile(g, tileCell(content, y, 0), amount, "amount");
    return tilesBottom(y, 1) + static_cast<float>(theme::space::groupGap);
}
}  // namespace

float paintRegionTabs(ParameterPage& page, juce::Graphics& g, juce::Rectangle<float> content, float y) {
    auto& model = page.model();
    const juce::Rectangle<float> area{content.getX(), y, content.getWidth(), ctl::segmentHeight};
    page.paintSegments(g, area, {"shape", "transform"}, std::clamp(model.regionTab(), 0, 1), true,
                       [&model](int choice) {
                           model.setRegionTab(choice);
                           model.notifyChanged();
                       });
    return area.getBottom() + static_cast<float>(theme::space::groupGap);
}

float paintRegionEditor(ParameterPage& page, juce::Graphics& g, juce::Rectangle<float> content, float y,
                        const RegionSlot& slot) {
    if (std::clamp(page.model().regionTab(), 0, 1) == 1) return paintTransformTab(page, g, content, y, slot);

    auto& model = page.model();
    const auto id = [&](const char* field) {
        return static_cast<ParamId>(model.manifest().byKey(slot.prefix + "." + field));
    };
    const auto shape = model.state().regions[static_cast<std::size_t>(slot.index)].shape;

    //  ---- kind: one row, the shapes then custom; state, so an undoable edit -----------------------
    //  Custom is a flag over the kept shape, greyed until weights exist. Picking a shape leaves the
    //  custom where it is; picking custom shows the kept one.
    const bool hasCustom = std::any_of(shape.weights.begin(), shape.weights.end(), [](double w) { return w != 0.0; });
    std::vector<juce::String> kindNames;
    for (const char* n : kRegionKindNames) kindNames.emplace_back(n);
    const auto kinds = page.paintPicker(
        g, content.getX(), y, content.getWidth(), kindNames,
        shape.custom ? kRegionCustomEntry : static_cast<int>(shape.kind), true,
        [&model, at = slot.index](int which) {
            model.applyEdit("region kind", [at, which](PluginState& s) {
                auto& r = s.regions[static_cast<std::size_t>(at)].shape;
                if (which == kRegionCustomEntry)
                    r.custom = true;
                else {
                    r.custom = false;
                    r.kind = static_cast<RegionKind>(which);
                }
                r = sanitised(r);  // its counts come with it, and stay evaluable
            });
            model.notifyChanged();
        },
        {}, hasCustom ? -1 : kRegionCustomEntry, 4);
    y = kinds.bottom + static_cast<float>(theme::space::groupGap);

    /*  What convert freezes: the kept shape at its zero pose, integrated against each harmonic. The
        same call answers a cell's double-click and `reset all`, so there is no second copy of the
        converted weights to keep in state. */
    //  at the plugin's order: what the bus plays is what is frozen, and nothing above it
    const auto projected = [&model, slot, shape] {
        bambi::Region kept = regionOf(model, slot);
        kept.kind = shape.kind;
        std::array<double, kRegionWeights> w{};
        projectWeights(kept, model.renderedOrder(), w);
        return w;
    };

    //  ---- shape, or the custom weights -------------------------------------------------------------
    if (shape.custom) {
        y = paintPyramid(page, g, content, y, slot, shape, projected);
    } else {
        if (shape.kind == RegionKind::Everywhere) {
            y = page.paintHint(g, content.withY(y), "the whole sphere: nothing to shape");
        } else {
            /*  The counts -- how many sectors, how many dots -- are state like the kind, because
                changing them changes what the other settings mean. They are drawn as segments beside
                the kind's own tiles rather than as values that can be dragged to nothing. */
            int cell = 0;
            if (shape.kind == RegionKind::Sectors || shape.kind == RegionKind::Dots) {
                const bool dots = shape.kind == RegionKind::Dots;
                const juce::Rectangle<float> counts{content.getX(), y, content.getWidth(), ctl::segmentHeight};
                if (dots) {
                    //  the six solids: a count that is not one of them has no even spread
                    page.paintSegments(
                        g, counts, {"2", "4", "6", "8", "12", "20"},
                        [&] {
                            static constexpr std::array<int, 6> kCounts{2, 4, 6, 8, 12, 20};
                            for (int i = 0; i < 6; ++i)
                                if (kCounts[static_cast<std::size_t>(i)] == shape.dots) return i;
                            return 2;
                        }(),
                        true,
                        [&model, at = slot.index](int which) {
                            static constexpr std::array<int, 6> kCounts{2, 4, 6, 8, 12, 20};
                            model.applyEdit("dots", [at, which](PluginState& s) {
                                auto& r = s.regions[static_cast<std::size_t>(at)].shape;
                                r.dots = kCounts[static_cast<std::size_t>(which)];
                                r = sanitised(r);
                            });
                            model.notifyChanged();
                        });
                } else {
                    page.paintSegments(
                        g, counts, {"2", "3", "4", "5", "6", "8"},
                        [&] {
                            static constexpr std::array<int, 6> kCounts{2, 3, 4, 5, 6, 8};
                            for (int i = 0; i < 6; ++i)
                                if (kCounts[static_cast<std::size_t>(i)] == shape.sectors) return i;
                            return 2;
                        }(),
                        true,
                        [&model, at = slot.index](int which) {
                            static constexpr std::array<int, 6> kCounts{2, 3, 4, 5, 6, 8};
                            model.applyEdit("sectors", [at, which](PluginState& s) {
                                auto& r = s.regions[static_cast<std::size_t>(at)].shape;
                                r.sectors = kCounts[static_cast<std::size_t>(which)];
                                r = sanitised(r);
                            });
                            model.notifyChanged();
                        });
                }
                y = counts.getBottom() + ctl::sectionGap;
            }

            if (shape.kind == RegionKind::Clouds) {
                //  the seed is state, set once: a tile dragged like the trajectory's own numbers
                const auto tile = tileCell(content, y, cell++);
                TileContent t;
                t.name = "seed";
                t.value = juce::String(shape.seed);
                t.fillTo = static_cast<double>(shape.seed - 1) / (kMaxCloudSeed - 1);
                drawTile(g, tile, t);
                page.addStateDrag(
                    tileBox(tile), "seed", slot.prefix + ".seed", shape.seed, kMaxCloudSeed - 1, 1.0,
                    [at = slot.index](PluginState& s, double v) {
                        s.regions[static_cast<std::size_t>(at)].shape.seed = static_cast<int>(v);
                    },
                    [at = slot.index](PluginState& s) { s.regions[static_cast<std::size_t>(at)].shape.seed = 1; }, {},
                    1.0, kMaxCloudSeed);
            }
            for (const char* field : shapeFields(shape.kind))
                page.addParameterTile(g, tileCell(content, y, cell++), id(field));
            //  a weights kind has no edge for softness to fade
            if (shape.kind != RegionKind::Clouds)
                page.addParameterTile(g, tileCell(content, y, cell++), id("softness"));
            y = tilesBottom(y, cell) + ctl::sectionGap;
        }

        //  convert to custom: the one way to weights. It always replaces the custom there was;
        //  undo is the way back, so nothing asks.
        const bool canConvert = shape.kind != RegionKind::Everywhere;
        const juce::Rectangle<float> convert{content.getX(), y, content.getWidth(), ctl::buttonHeight};
        page.paintButton(g, convert, "convert to custom", canConvert, [&model, at = slot.index, projected] {
            const auto w = projected();
            model.applyEdit("convert to custom", [at, w](PluginState& s) {
                auto& r = s.regions[static_cast<std::size_t>(at)].shape;
                r.weights = w;
                r.gains = RegionShape{}.gains;
                r.custom = true;
            });
            model.notifyChanged();
        });
        page.noteNamedArea("region convert", convert, canConvert);
        y = convert.getBottom() + ctl::segmentGap;
        if (hasCustom) {
            const auto line = theme::type::label * ctl::noticeLineHeight;
            text(g, "replaces the custom weights you have", labelFont(), colour::label,
                 {content.getX(), y, content.getWidth(), line});
            y += line;
        }
        y += static_cast<float>(theme::space::groupGap) - ctl::segmentGap;
    }

    //  ---- copy and paste: the clipboard, not an instance picker -------------------------------------
    y = page.paintGroupTitle(g, content, y, "copy");
    const auto half = (content.getWidth() - ctl::tileColumnGap) / 2.0f;
    const juce::Rectangle<float> copyArea{content.getX(), y, half, ctl::buttonHeight};
    const juce::Rectangle<float> pasteArea{content.getX() + half + ctl::tileColumnGap, y, half, ctl::buttonHeight};
    const auto held = regionOnClipboard();
    /*  Not onto another instance. A paste is one edit and then a gesture a parameter -- a hundred
        commands in one turn, into an inbox of thirty-two, and the ones that did not fit would be
        lost without a word. Until a paste can reach another instance as one message it stays this
        window's own. Copy reads, and works on either. */
    const bool own = !model.showingAnother();
    const bool canPaste = held.has_value() && own;
    page.notePasteShown(held.has_value() ? describeRegionClip(*held) : std::string());

    page.paintButton(g, copyArea, "copy", true, [&model, slot] {
        copyToClipboard(copyRegion(model.modManifest(), model.state(), slot.index, slot.prefix));
        model.notifyChanged();  // the paste button and its readout follow what is on the clipboard
    });
    page.noteNamedArea("region copy", copyArea, true);

    //  paste is greyed on a clipboard that holds no current region, rather than guessing at what an
    //  older or a foreign payload meant
    page.paintButton(g, pasteArea, "paste", canPaste, [&model, slot] {
        const auto clip = regionOnClipboard();
        if (!clip.has_value()) return;  // it changed between the paint and the click
        //  what is a host parameter is worked out on a copy, and written through the host's own
        PluginState scratch = model.state();
        std::vector<std::pair<int, float>> writes;
        pasteRegion(model.modManifest(), *clip, slot.index, slot.prefix, scratch, writes);

        //  what is state -- the shape, the rows -- is one undoable edit
        const auto* mods = &model.modManifest();
        model.applyEdit("paste region", [mods, held = *clip, slot](PluginState& s) {
            std::vector<std::pair<int, float>> unused;
            pasteRegion(*mods, held, slot.index, slot.prefix, s, unused);
        });
        const auto& m = model.manifest();
        for (const auto& [at, value] : writes)
            model.changeParameter(static_cast<ParamId>(at), toNormalised(m, at, value));
        //  a pasted region sits where its numbers say: the turn this one had added is not its own
        for (int axis = 0; axis < 3; ++axis) model.zeroRegionTurn(slot.index, axis);
        model.notifyChanged();
    });
    page.noteNamedArea("region paste", pasteArea, canPaste);
    y = copyArea.getBottom() + ctl::segmentGap;

    //  what a paste would bring, so that it is not a blind trust-fall
    const auto line = theme::type::label * ctl::noticeLineHeight;
    text(g,
         !held.has_value() ? juce::String("the clipboard holds no region")
         : !own            ? juce::String("paste is for this window's own instance")
                           : "clipboard: " + fromCore(describeRegionClip(*held)),
         labelFont(), colour::label, {content.getX(), y, content.getWidth(), line});
    y += line;
    page.noteContentBottom(y + page.scroll());
    return y;
}

namespace {
/*  A slot's settings, read from the parameters in the units the parameters have: degrees. There is
    no second place that knows this mapping -- `resolveRegion` turns it into radians, as it does for
    the audio thread. */
RegionSettingsDeg settingsOf(const PatchModel& model, const RegionSlot& slot) {
    const auto& m = model.manifest();
    const auto of = [&](const char* field, double fallback) {
        const int at = m.byKey(slot.prefix + "." + field);
        return at == kNoParam ? fallback : static_cast<double>(model.valueOf(static_cast<ParamId>(at)));
    };
    RegionSettingsDeg d;
    d.yaw = of("yaw", 0.0);
    d.pitch = of("pitch", 0.0);
    d.roll = of("roll", 0.0);
    d.softness = of("softness", 20.0);
    d.size = of("size", 45.0);
    d.bandElevation = of("band_elevation", 0.0);
    d.thickness = of("thickness", 30.0);
    d.fill = of("fill", 0.5);
    d.dotSize = of("dot_size", 20.0);
    d.coverage = of("coverage", 0.5);
    d.contrast = of("contrast", 0.6);
    d.detail = of("detail", 0.5);
    d.evolve = of("evolve", 0.0);
    return d;
}

/// Write one of the slot's parameters, in its own units, as a gesture of one.
void write(PatchModel& model, const RegionSlot& slot, const char* field, double value) {
    const int at = model.manifest().byKey(slot.prefix + "." + field);
    if (at == kNoParam) return;
    const auto id = static_cast<ParamId>(at);
    model.changeParameter(
        id, toNormalised(model.manifest(), at, clampToRange(model.manifest(), at, static_cast<float>(value))));
}

constexpr double kDeg = 180.0 / kPi;

/// Wrapped into (-180, 180]: an angle written back is a setting, and a setting does not wind up.
double wrapDeg(double d) {
    d = std::fmod(d + 180.0, 360.0);
    return (d < 0.0 ? d + 360.0 : d) - 180.0;
}
}  // namespace

bambi::Region regionOf(const PatchModel& model, const RegionSlot& slot, double yawTurnRad, double pitchTurnRad,
                       double rollTurnRad) {
    const auto& entry = model.state().regions[static_cast<std::size_t>(slot.index)];
    const int sideAt = model.manifest().byKey(slot.prefix + ".side");
    const auto side = sideAt != kNoParam && model.valueOf(static_cast<ParamId>(sideAt)) > 0.5f ? RegionSide::Outside
                                                                                               : RegionSide::Inside;
    return resolveRegion(sanitised(entry.shape), side, settingsOf(model, slot), yawTurnRad, pitchTurnRad, rollTurnRad);
}

void carryRegionHandle(PatchModel& model, const RegionSlot& slot, const bambi::Region& now, RegionHandle::Kind kind,
                       Vec3 to, double yawTurnRad, double pitchTurnRad, double rollTurnRad) {
    const auto aim = aimOf(now);
    const auto angle = std::acos(std::clamp(dot(unit(to), aim), -1.0, 1.0));

    switch (kind) {
        case RegionHandle::Kind::Aim: {
            /*  `aimAt` solves yaw and pitch -- and rewrites roll where a turn about the axis can be
                seen -- on a copy of the drawn region, and what is written back is the copy's angles
                less the turn a rate has already added. Writing the drawn angle would add the turn
                twice and jump the region by everything it has turned so far. */
            bambi::Region turned = now;
            aimAt(turned, unit(to));
            write(model, slot, "yaw", wrapDeg(turned.yaw * kDeg - yawTurnRad * kDeg));
            write(model, slot, "pitch", wrapDeg(turned.pitch * kDeg - pitchTurnRad * kDeg));
            if (turned.roll != now.roll) write(model, slot, "roll", wrapDeg(turned.roll * kDeg - rollTurnRad * kDeg));
            break;
        }
        case RegionHandle::Kind::Roll: {
            /*  The angle about the aim, measured in the region's own frame, less where the handle stands
                about it: 0 for sectors, the reference dot's for dots. Pressed where it is drawn, it
                moves nothing. */
            const auto own = toOwnFrame(now, unit(to));
            const auto ref = now.kind == RegionKind::Dots ? referenceDot(now.dots) : std::optional<Vec3>{};
            const auto stands = ref.has_value() ? std::atan2(ref->y, ref->x) : 0.0;
            const auto around = std::atan2(own.y, own.x) - stands;
            write(model, slot, "roll", wrapDeg((now.roll + around) * kDeg - rollTurnRad * kDeg));
            break;
        }
        case RegionHandle::Kind::Elevation:
            //  A band's centre, carried by the colatitude the pointer sits at.
            write(model, slot, "band_elevation", wrapDeg((kPi / 2.0 - angle) * kDeg));
            break;
        case RegionHandle::Kind::Edge:
            switch (now.kind) {
                case RegionKind::Spot: write(model, slot, "size", angle * kDeg); break;
                case RegionKind::Dots: {
                    //  from the dot it sizes, which is where its handle stands -- the axis only for a pair
                    const auto ref = referenceDot(now.dots);
                    const auto centre = ref.has_value() ? *ref : Vec3{0.0, 0.0, 1.0};
                    const auto own = toOwnFrame(now, unit(to));
                    write(model, slot, "dot_size", std::acos(std::clamp(dot(own, centre), -1.0, 1.0)) * kDeg);
                    break;
                }
                case RegionKind::Band:
                    //  Half the thickness away from the centre, so the edge lands where it is carried.
                    write(model, slot, "thickness", 2.0 * std::abs(kPi / 2.0 - now.bandElevation - angle) * kDeg);
                    break;
                case RegionKind::Sectors: {
                    /*  A sector's edge is half its filled arc from its centre line, and the arc is
                        measured about the aim: the colatitude says nothing about fill. */
                    //  the own frame already carries the roll, as the pattern does
                    const auto own = toOwnFrame(now, unit(to));
                    const auto around = std::atan2(own.y, own.x);
                    const auto period = 2.0 * kPi / std::max(1, now.sectors);
                    auto half = std::fmod(std::abs(around), period);
                    if (half > period / 2.0) half = period - half;
                    write(model, slot, "fill", std::clamp(2.0 * half / period, 0.0, 1.0));
                    break;
                }
                default: break;
            }
            break;
    }
    model.notifyChanged();
}

void resetRegionHandle(PatchModel& model, const RegionSlot& slot, const bambi::Region& now, RegionHandle::Kind kind) {
    //  the manifest's own default, so the scene and the tile cannot disagree about what one is
    const auto toDefault = [&](const char* field) {
        const int at = model.manifest().byKey(slot.prefix + "." + field);
        if (at != kNoParam) write(model, slot, field, static_cast<double>(model.manifest()[at].def));
    };
    /*  A handle back to its default clears the turn too, exactly as its tile does: the tile goes
        through `ParameterPage::onDefaultRestored` and a handle does not touch that path, so it says
        the same thing here. */
    switch (kind) {
        case RegionHandle::Kind::Aim:
            write(model, slot, "yaw", 0.0);
            write(model, slot, "pitch", 0.0);
            model.zeroRegionTurn(slot.index, 0);
            model.zeroRegionTurn(slot.index, 1);
            break;
        case RegionHandle::Kind::Roll:
            write(model, slot, "roll", 0.0);
            model.zeroRegionTurn(slot.index, 2);
            break;
        case RegionHandle::Kind::Elevation: toDefault("band_elevation"); break;
        case RegionHandle::Kind::Edge:
            switch (now.kind) {
                case RegionKind::Spot: toDefault("size"); break;
                case RegionKind::Band: toDefault("thickness"); break;
                case RegionKind::Sectors: toDefault("fill"); break;
                case RegionKind::Dots: toDefault("dot_size"); break;
                default: break;
            }
            break;
    }
    model.notifyChanged();
}

bool regionAngleOf(const ParamManifest& m, ParamId id, int& slot, int& axis) {
    if (id == kNoParamId || !m.has(static_cast<int>(id))) return false;
    const auto key = m[static_cast<int>(id)].key;
    if (key.rfind("region", 0) != 0) return false;
    const auto dot = key.find('.');
    if (dot == std::string_view::npos) return false;

    //  `region1`, `region2`: the number after the word, one-based, as the key block stamps it
    const auto digits = key.substr(6, dot - 6);
    int number = 0;
    for (const char c : digits) {
        if (c < '0' || c > '9') return false;
        number = number * 10 + (c - '0');
    }
    if (number < 1 || number > kMaxRegions) return false;

    const auto field = key.substr(dot + 1);
    if (field == "yaw")
        axis = 0;
    else if (field == "pitch")
        axis = 1;
    else if (field == "roll")
        axis = 2;
    else
        return false;  // a rate is not an angle: `yaw_rate` stops here
    slot = number - 1;
    return true;
}

}  // namespace bambi::ui
