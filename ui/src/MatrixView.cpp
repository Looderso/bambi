// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/ui/MatrixView.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>

#include "bambi/ui/Draw.h"
#include "bambi/ui/RegionEditor.h"
#include "bambi/ui/Widgets.h"

namespace bambi::ui {

namespace {
namespace colour = theme::colour;
namespace ctl = theme::controls;
namespace m = theme::metrics;

constexpr std::array<const char*, 4> kTabNames{"features", "sidechain", "generators", "region"};
constexpr std::array<bambi::MatrixTab, 4> kTabs{bambi::MatrixTab::Features, bambi::MatrixTab::Sidechain,
                                                bambi::MatrixTab::Generators, bambi::MatrixTab::Region};

juce::String columnSub(const MatrixModel& model, bambi::MatrixTab tab, int column) {
    const auto& patch = model.state();
    switch (tab) {
        case bambi::MatrixTab::Features: return "self";
        case bambi::MatrixTab::Sidechain: return "sc";
        case bambi::MatrixTab::Generators:
            if (column < bambi::kNumLfos) return "gen";
            return patch.envTriggers[static_cast<std::size_t>(column - bambi::kNumLfos)].input ==
                           bambi::TriggerInput::Midi
                       ? "midi"
                       : "audio";
        case bambi::MatrixTab::Region: {
            //  what it is and which side passes -- "spot . inside" -- the only two facts that change
            //  what the source's value means. The kind is state; the side is a parameter, and both
            //  are read where they live
            const auto slot = model.regionSlot();
            const auto& shape = patch.regions[static_cast<std::size_t>(slot.index)].shape;
            const auto kind = kRegionKindNames[static_cast<std::size_t>(
                shape.custom
                    ? kRegionCustomEntry
                    : std::clamp(static_cast<int>(shape.kind), 0, static_cast<int>(kRegionKindNames.size()) - 1))];
            const int sideAt = model.manifest().byKey(slot.prefix + ".side");
            const bool outside = sideAt != bambi::kNoParam && model.valueOf(static_cast<bambi::ParamId>(sideAt)) > 0.5f;
            return juce::String(kind) + " " + juce::String(juce::CharPointer_UTF8("\xc2\xb7")) + " " +
                   (outside ? "outside" : "inside");
        }
    }
    return {};
}
}  // namespace

MatrixView::MatrixView(MatrixModel& model) : model_(model) { setOpaque(true); }

juce::Rectangle<float> MatrixView::columnHeadArea(int column) const {
    return column >= 0 && column < static_cast<int>(columnHeads_.size())
               ? columnHeads_[static_cast<std::size_t>(column)]
               : juce::Rectangle<float>{};
}

juce::Rectangle<float> MatrixView::tabArea(bambi::MatrixTab tab) const {
    const auto at = static_cast<std::size_t>(tab);
    return at < tabAreas_.size() ? tabAreas_[at] : juce::Rectangle<float>{};
}

std::vector<bambi::MatrixTab> MatrixView::tabsOffered() const {
    std::vector<bambi::MatrixTab> offered;
    for (const auto tab : kTabs)
        if (tab != bambi::MatrixTab::Region || model_.regionIsSource()) offered.push_back(tab);
    return offered;
}

void MatrixView::repaintHeader() {
    ++headerRepaints_;
    repaint(0, 0, getWidth(), juce::roundToInt(ctl::tabBarHeight + ctl::matrixHeader));
}

void MatrixView::paint(juce::Graphics& g) {
    clearRegions();
    g.fillAll(colour::ground);

    const auto width = static_cast<float>(getWidth());
    const auto height = static_cast<float>(getHeight());
    const auto inset = static_cast<float>(theme::space::inset);
    const auto& patch = model_.state();

    //  the tabs, and how many targets the matrix has
    const auto tabFont = font(theme::type::body, theme::type::trackingTab);
    const auto tabHeight = ctl::tabBarHeight - ctl::matrixTabsTop;
    auto x = inset;
    tabAreas_.fill({});
    const auto offered = tabsOffered();
    //  a tab this plugin does not offer cannot be the current one: `setCurrentTab` is unguarded in
    //  every plugin and `showControls` takes a tab, so an effect can be put on the region's tab --
    //  and would then draw its one column with no tab open and no way back. Falling to the
    //  first tab is what the bar already says is there
    if (std::find(offered.begin(), offered.end(), model_.currentTab()) == offered.end())
        model_.setCurrentTab(bambi::MatrixTab::Features);
    for (const auto tab : offered) {
        const auto i =
            static_cast<std::size_t>(std::distance(kTabs.begin(), std::find(kTabs.begin(), kTabs.end(), tab)));
        const juce::Rectangle<float> box{x - ctl::tabPadX, ctl::matrixTabsTop,
                                         textWidth(tabFont, kTabNames[i]) + 2.0f * ctl::tabPadX, tabHeight};
        drawChoice(g, box, kTabNames[i], tabFont, tab == model_.currentTab());
        tabAreas_[static_cast<std::size_t>(tab)] = box;
        addRegion({box,
                   {},
                   {},
                   {},
                   [this, tab] {
                       model_.setCurrentTab(tab);
                       model_.notifyChanged();
                   },
                   {}});
        x = box.getRight() + ctl::tabGap + ctl::tabPadX;
    }
    const auto rows = bambi::matrixTargets(model_.modManifest(), patch);
    //  Rows are offered; targets are routed. The base three have a row from the start and no depth yet.
    const auto routed = static_cast<int>(
        std::count_if(rows.begin(), rows.end(), [&](bambi::ParamId id) { return bambi::targetHasDepth(patch, id); }));
    text(g, juce::String(routed) + (routed == 1 ? " target" : " targets"), labelFont(), colour::label,
         {inset, ctl::matrixTabsTop, width - 2.0f * inset, tabHeight}, juce::Justification::centredRight);

    const auto top = ctl::tabBarHeight;
    setViewport(top + ctl::matrixHeader, height - static_cast<float>(m::footerHeight));
    if (model_.showingAnother() && !model_.anotherReady()) {
        const juce::Rectangle<float> area{inset, top, width - 2.0f * inset,
                                          height - top - static_cast<float>(m::footerHeight)};
        drawRemoteNotice(g, area, model_.anotherLabel());
        addRegion({remoteNoticeLink(area), {}, {}, {}, [this] { model_.selectSelfInstance(); }, {}});
        return;
    }

    //  column heads: name, what it listens to, and its amount
    const auto targetsWidth = static_cast<float>(m::matrixTargets);
    const auto columnsX = inset + targetsWidth;
    //  the column width is always a sixth, whatever the tab has: the region's one column keeps the
    //  width every other column has rather than stretching across the whole matrix, so the grid does
    //  not change shape when the tab does
    const auto columnWidth = (width - 2.0f * inset - targetsWidth) / static_cast<float>(bambi::kSourcesPerTab);
    const auto columns = bambi::columnsIn(model_.currentTab());
    columnsDrawn_ = columns;
    columnHeads_.fill({});
    text(g, "target", labelFont(), colour::label, {inset, top, targetsWidth, ctl::matrixHeader - ctl::matrixHeadPad},
         juce::Justification::bottomLeft);
    for (int column = 0; column < columns; ++column) {
        const auto source = bambi::sourceColumn(model_.modManifest(), model_.currentTab(), column);
        const auto cx = columnsX + static_cast<float>(column) * columnWidth;
        const bool open = model_.sourceOpen(bambi::sourceSlot(model_.currentTab(), column));
        text(g, fromCore(source.name), font(theme::type::matrix), open ? colour::selected() : colour::text,
             {cx, top + ctl::matrixNameTop, columnWidth, theme::type::matrix * ctl::valueLineHeight},
             juce::Justification::centred);
        text(g, columnSub(model_, model_.currentTab(), column), smallFont(), colour::label,
             {cx, top + ctl::matrixSubTop, columnWidth, theme::type::small * ctl::valueLineHeight},
             juce::Justification::centred);

        //  What this source is sending right now, in its own range: what a depth here would actually do,
        //  before you set one. Bipolar for the LFOs, which swing about a middle they genuinely have.
        const auto slot = bambi::sourceSlot(model_.currentTab(), column);
        const auto live = static_cast<double>(model_.sourceValueOf(slot));
        BarContent liveBar;
        liveBar.fillFrom = source.bipolar ? 0.5 : 0.0;
        liveBar.fillTo = source.bipolar ? 0.5 + 0.5 * std::clamp(live, -1.0, 1.0) : std::clamp(live, 0.0, 1.0);
        liveBar.tick = source.bipolar;
        liveBar.ink = colour::output();
        drawValueBar(
            g,
            {cx + ctl::cellInset, top + ctl::matrixLiveTop, columnWidth - 2.0f * ctl::cellInset, ctl::matrixLiveHeight},
            liveBar);
        const juce::Rectangle<float> head{cx, top, columnWidth, ctl::matrixAmountTop - ctl::barGrab};
        columnHeads_[static_cast<std::size_t>(column)] = head;
        addRegion({head, {}, {}, {}, [this, slot] { model_.openSourceSettings(slot); }, {}});

        const juce::Rectangle<float> bar{cx + ctl::cellInset, top + ctl::matrixAmountTop,
                                         columnWidth - 2.0f * ctl::cellInset, ctl::barHeight};
        const float amount = model_.valueOf(source.amount);
        BarContent amountBar;
        amountBar.fillTo = std::clamp(amount, 0.0f, 1.0f);
        amountBar.ink = colour::modulation();
        drawValueBar(g, bar, amountBar);
        text(g, juce::String(juce::roundToInt(amount * 100.0f)) + " %", smallFont(), colour::modulation(),
             {cx, bar.getBottom() + ctl::matrixAmountGap, columnWidth, theme::type::small * ctl::valueLineHeight},
             juce::Justification::centred);

        const auto amountId = source.amount;
        const auto setAmount = [this, amountId, bar](juce::Point<float> p) {
            model_.setParameter(amountId, juce::jlimit(0.0f, 1.0f, (p.x - bar.getX()) / bar.getWidth()));
        };
        addRegion({bar.expanded(0.0f, ctl::barGrab),
                   [this, amountId, setAmount](juce::Point<float> p) {
                       model_.beginParameter(amountId);
                       setAmount(p);
                   },
                   [setAmount](juce::Point<float> p, bool) { setAmount(p); },
                   [this, amountId] { model_.endParameter(amountId); },
                   {},
                   {}});
    }
    ruleH(g, inset, width - inset, top + ctl::matrixHeader, colour::rule);

    //  the rows: every target with depth, and the provisional one
    std::vector<bambi::ParamId> shown = rows;
    const bool ghost = model_.provisionalRow() != bambi::kNoParamId &&
                       std::find(rows.begin(), rows.end(), model_.provisionalRow()) == rows.end();
    if (ghost) shown.push_back(model_.provisionalRow());

    const auto rowHeight = static_cast<float>(m::matrixRow);
    const auto cellHeight = static_cast<float>(m::matrixCell);

    //  the rows scroll; the column heads do not. Heads that scrolled away would leave every cell unreadable,
    //  and shifting where the rows start carries their click regions along, because a region is registered
    //  where it is drawn
    const auto viewTop = top + ctl::matrixHeader;
    std::optional<ContentClip> clip;
    clip.emplace(*this, g);
    auto y = viewTop - scroll();
    for (const auto id : shown) {
        const juce::Rectangle<float> row{inset, y, width - 2.0f * inset, rowHeight};
        const auto name = fromCore(model_.desc(id).name);  // this plugin's manifest, never the encoder's
        const auto nameFont = font(theme::type::matrix);
        text(g, name, nameFont, colour::text, {inset, y, targetsWidth, rowHeight});
        if (bambi::hasDepthElsewhere(patch, id, model_.currentTab())) {
            const auto d = 2.0f * theme::shape::elsewhereDot;
            g.setColour(colour::elsewhere);
            g.fillEllipse(inset + textWidth(nameFont, name) + ctl::elsewhereGap, row.getCentreY() - d / 2.0f, d, d);
        }

        for (int column = 0; column < columns; ++column) {
            const auto cx = columnsX + static_cast<float>(column) * columnWidth;
            const juce::Rectangle<float> cell{cx + ctl::cellInset, row.getCentreY() - cellHeight / 2.0f,
                                              columnWidth - 2.0f * ctl::cellInset, cellHeight};
            const auto depth = static_cast<float>(bambi::cellDepth(patch, model_.currentTab(), column, id));
            if (juce::exactlyEqual(depth, 0.0f)) {
                g.setColour(colour::emptyCell);
                g.fillRect(cell);
            } else {
                //  from the middle, along the cell's long side: the same bar every other value uses, so a
                //  sign change slides across zero instead of jumping from one edge to the other
                BarContent bar;
                bar.fillFrom = 0.5;
                bar.fillTo = 0.5 + 0.5 * static_cast<double>(std::clamp(depth, -1.0f, 1.0f));
                bar.tick = true;
                bar.ink = colour::modulation();
                drawValueBar(g, cell, bar);
                //  In the cell's own empty half, so it never sits on the fill or reaches the next column.
                const auto half = depth < 0.0f ? cell.withTrimmedLeft(cell.getWidth() / 2.0f)
                                               : cell.withWidth(cell.getWidth() / 2.0f);
                text(g, fromCore(bambi::formatNumber(static_cast<double>(depth), 2)), smallFont(), colour::text,
                     half.reduced(ctl::cellTextInset, 0.0f),
                     depth < 0.0f ? juce::Justification::centredRight : juce::Justification::centredLeft);
            }

            const auto tab = model_.currentTab();
            const auto key = "matrix." + std::to_string(static_cast<int>(tab)) + "." + std::to_string(column) + "." +
                             std::to_string(static_cast<int>(id));
            const auto& mods = model_.modManifest();
            const auto set = [&mods, tab, column, id](double to) {
                return [&mods, tab, column, id, to](bambi::PluginState& s) {
                    bambi::setCellDepth(mods, s, tab, column, id, to);
                };
            };
            addRegion({cell.withY(row.getY()).withHeight(rowHeight),
                       [this, tab, column, id](juce::Point<float> p) {
                           dragStart_ = static_cast<float>(bambi::cellDepth(model_.state(), tab, column, id));
                           dragY_ = p.y;
                       },
                       [this, tab, column, id, key, set](juce::Point<float> p, bool fine) {
                           const auto delta = (dragY_ - p.y) / ctl::depthPixels * (fine ? ctl::fineDrag : 1.0f);
                           const auto next = std::round(std::clamp(dragStart_ + delta, -1.0f, 1.0f) * 100.0f) / 100.0f;
                           if (!juce::exactlyEqual(static_cast<double>(next),
                                                   bambi::cellDepth(model_.state(), tab, column, id)))
                               model_.applyEditDrag("matrix depth", key, set(static_cast<double>(next)));
                       },
                       [this] { model_.finishDrag(); },
                       [this, tab, column, id, set] {
                           const bool empty =
                               juce::exactlyEqual(bambi::cellDepth(model_.state(), tab, column, id), 0.0);
                           model_.applyEdit("matrix depth", set(empty ? static_cast<double>(ctl::defaultDepth) : 0.0));
                       },
                       [this, set] { model_.applyEdit("matrix depth", set(0.0)); }});
        }

        ruleH(g, inset, width - inset, row.getBottom() - theme::stroke::rule, colour::rowDivider);
        if (ghost && id == model_.provisionalRow()) {
            juce::Path outline, dashes;
            outline.addRectangle(row);
            juce::PathStrokeType(theme::stroke::rule).createDashedStroke(dashes, outline, ctl::provisionalDash, 2);
            g.setColour(colour::provisional);
            g.fillPath(dashes);
        }
        y += rowHeight;
    }

    text(g,
         "provisional row " + juce::String::fromUTF8("\xe2\x80\x94") +
             " clicking another parameter replaces it. any depth keeps it.",
         labelFont(), colour::label.withMultipliedAlpha(ghost ? 1.0f : ctl::hintAlpha),
         {inset, y + ctl::hintGap, width - 2.0f * inset, theme::type::label * ctl::noticeLineHeight});
    noteContentBottom(y + ctl::hintGap + theme::type::label * ctl::noticeLineHeight + scroll());
    clip.reset();  // the footer is pinned, like the heads

    paintFooter(g);
}

void MatrixView::paintFooter(juce::Graphics& g) {
    const auto width = static_cast<float>(getWidth());
    const auto inset = static_cast<float>(theme::space::inset);
    const auto footerHeight = static_cast<float>(m::footerHeight);
    const juce::Rectangle<float> footer{0.0f, static_cast<float>(getHeight()) - footerHeight, width, footerHeight};
    ruleH(g, 0.0f, width, footer.getY(), colour::rule);

    const auto label = juce::String("global amount");
    const auto labelWidth = textWidth(labelFont(), label) + 2.0f;
    text(g, label, labelFont(), colour::label, {inset, footer.getY(), labelWidth, footerHeight});

    const juce::Rectangle<float> bar{inset + labelWidth + ctl::footerGap, footer.getCentreY() - ctl::barHeight / 2.0f,
                                     ctl::globalBarWidth, ctl::barHeight};
    const auto global = model_.valueOf(static_cast<bambi::ParamId>(model_.modManifest().globalAmount));
    BarContent globalBar;
    globalBar.fillTo = std::clamp(global, 0.0f, 1.0f);
    globalBar.ink = colour::modulation();
    drawValueBar(g, bar, globalBar);
    text(g, juce::String(juce::roundToInt(global * 100.0f)) + " %", font(theme::type::matrix), colour::text,
         {bar.getRight() + ctl::footerGap, footer.getY(), ctl::globalBarWidth, footerHeight});

    const auto setGlobal = [this, bar](juce::Point<float> p) {
        model_.setParameter(static_cast<bambi::ParamId>(model_.modManifest().globalAmount),
                            juce::jlimit(0.0f, 1.0f, (p.x - bar.getX()) / bar.getWidth()));
    };
    addRegion({bar.expanded(0.0f, ctl::barGrab),
               [this, setGlobal](juce::Point<float> p) {
                   model_.beginParameter(static_cast<bambi::ParamId>(model_.modManifest().globalAmount));
                   setGlobal(p);
               },
               [setGlobal](juce::Point<float> p, bool) { setGlobal(p); },
               [this] { model_.endParameter(static_cast<bambi::ParamId>(model_.modManifest().globalAmount)); },
               {},
               {}});

    const auto note = juce::String("has depth on another tab");
    const auto noteWidth = textWidth(labelFont(), note) + 2.0f;
    const auto noteX = width - inset - noteWidth;
    text(g, note, labelFont(), colour::label, {noteX, footer.getY(), noteWidth, footerHeight});
    const auto d = 2.0f * theme::shape::elsewhereDot;
    g.setColour(colour::elsewhere);
    g.fillEllipse(noteX - ctl::elsewhereGap - d, footer.getCentreY() - d / 2.0f, d, d);
}

}  // namespace bambi::ui
