// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <functional>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "bambi/ui/EnvelopeGraph.h"
#include "bambi/ui/HitArea.h"
#include "bambi/ui/HostDrag.h"
#include "bambi/ui/PatchModel.h"
#include "bambi/ui/Tiles.h"

/*  A page of parameters, as every plugin in the suite draws one: group titles with a rule under them,
 *  a grid of tiles two to a row, segmented controls for a choice, a hint line, a framed button for an
 *  action -- dragged vertically, shift for fine, double-click for default, click a name for a matrix
 *  row. What is a plugin's own is which parameters go on which page, in what order, under what titles.
 *
 *  It knows no plugin's parameter enum: a `ParamId` is opaque here, as it is everywhere in `ui/`.
 */
namespace bambi::ui {

/// A source's amount reads as a percentage: the modulation group's parameters that carry no unit. One
/// What a tile shows for a value, as text.
juce::String tileValueText(const ParamDesc& d, double value);

class ParameterPage : public HitArea {
public:
    explicit ParameterPage(PatchModel& model) : model_(model) {}

    /// Where a parameter's value box was last drawn, and whether it was drawn live. Empty for a
    /// parameter this page does not show. Public so a headless check can click it. `drawnArea`
    /// records every tile, greyed or not; `nameArea` is empty for a greyed tile, which takes no click.
    juce::Rectangle<float> drawnArea(ParamId id) const {
        return id == kNoParamId ? juce::Rectangle<float>{} : tileAreas_[static_cast<std::size_t>(id)];
    }
    bool tileIsEnabled(ParamId id) const { return id != kNoParamId && tileEnabled_[static_cast<std::size_t>(id)]; }
    juce::Rectangle<float> nameArea(ParamId id) const {
        return tileIsEnabled(id) ? drawnArea(id) : juce::Rectangle<float>{};
    }

public:
    PatchModel& model() { return model_; }
    const PatchModel& model() const { return model_; }

    /*  The frame every plugin's right-hand panel has: a row of tabs with the open one a black block, a
     *  rule under them, and everything they govern scrolling beneath while the bar stays put. Returns
     *  the content rect to draw into, already scrolled -- or an empty rect when another instance is
     *  selected and has not answered yet, in which case the notice has been drawn and there is
     *  nothing to paint. `extra` is drawn after the tabs and returns the x it ended at, for a plugin
     *  with something more in the bar. The caller clips and restores; `paintFrame` leaves the
     *  graphics state as it found it. */
    struct FrameContent {
        juce::Rectangle<float> content;  ///< empty when the remote notice took the panel
        juce::Rectangle<float> full;     ///< unscrolled, for a caller that needs the panel's own rect
    };
    FrameContent paintFrame(juce::Graphics& g, std::initializer_list<const char*> tabs, int selected,
                            const std::function<void(int)>& select,
                            const std::function<float(juce::Graphics&, float x)>& extra = {});

    /*  A free-or-synced switch, drawn as two segments. Returns true when it is synced. Both timings
     *  stay on screen whatever it says; the inactive one is greyed and inert, so the panel does not
     *  change shape and the other mode stays visible. Pass the result as `enabled` to the tiles on
     *  each side. The layout is deliberately not here: panels arrange these timings differently. */
    bool paintSyncSwitch(juce::Graphics& g, juce::Rectangle<float> area, ParamId sync);

    /// Restart or continue, as two segments, and under them -- only while it says continue -- the
    /// line that says what that costs: it runs on through play and locate, so a bounce will differ.
    /// `enabled` false draws it greyed and inert. Returns the y to carry on at.
    float paintRetrigger(juce::Graphics& g, juce::Rectangle<float> content, float y, ParamId retrigger,
                         bool enabled = true);

    /// The same frame with the temporary tab a source's settings open in, after the plugin's own:
    /// named for the source, with chevrons to step between sources and a cross back to the tab it
    /// replaced. Everything it needs is the model's, so every panel draws it by calling this.
    FrameContent paintTabbedFrame(juce::Graphics& g, std::initializer_list<const char*> tabs);

    /// The temporary fourth tab: a source's settings, opened from a matrix column, with chevrons to
    /// step between sources and a cross back to the tab it replaced. `paintTabbedFrame` builds one
    /// from the model and passes it to `paintFrame` as its `extra`.
    struct SourceTab {
        juce::String name;
        bool open{false};
        std::function<void()> select, close, previous, next;
    };
    float paintSourceTab(juce::Graphics& g, float x, const SourceTab& tab);

    /// The envelope's graph, which holds a gesture across the paints it causes. It lives on the page
    /// because the page is what survives one, and because a plugin has one envelope open at a time
    /// whichever of the three it is.
    EnvelopeGraph& envelopeGraph() { return envelope_; }
    const EnvelopeGraph& envelopeGraph() const { return envelope_; }

    /// Where the two chevrons were last drawn, for a check that clicks them.
    const std::array<juce::Rectangle<float>, 2>& sourceStepAreas() const { return stepAreas_; }

    //  ---- the pieces a page is built from -------------------------------------------------
public:
    /*  A line of explanation under a control. Wraps onto a second line and no further: a hint that
     *  needs three is a hint that should be shorter. Returns the y to carry on at. */
    float paintHint(juce::Graphics& g, juce::Rectangle<float> content, const juce::String& hint);

    /// A group's name with a rule under it. Returns the y to carry on at.
    float paintGroupTitle(juce::Graphics& g, juce::Rectangle<float> content, float y, const juce::String& title);

    /*  One slot of a group's grid: a parameter, or a tile the page draws itself -- the encoder's
     *  width, whose two ends are one box. A custom slot is named at the call site rather than
     *  signalled by a sentinel id, so a page may have two of them and neither is `kNoParamId`. */
    struct Cell {
        ParamId id{kNoParamId};
        std::function<void(juce::Graphics&, juce::Rectangle<float>)> custom;

        Cell(ParamId at) : id(at) {}
        Cell(std::function<void(juce::Graphics&, juce::Rectangle<float>)> draw) : custom(std::move(draw)) {}
    };

    /// A title and a grid of tiles, two to a row. Returns the y after the group's gap.
    float paintGroup(juce::Graphics& g, juce::Rectangle<float> content, float y, const juce::String& title,
                     std::initializer_list<Cell> cells);

    /*  A choice as segments. `unavailable` is a segment drawn greyed that takes no click -- an
     *  option this state cannot reach, shown rather than hidden so the set does not change shape. */
    void paintSegments(juce::Graphics& g, juce::Rectangle<float> area, std::initializer_list<const char*> names,
                       int selected, bool enabled, const std::function<void(int)>& choose, int unavailable = -1);

    /*  The picker: one row of entries, wrapped at `columns` a line, the chosen one filled. `choose`
     *  takes a click on another entry; `reselect`, when given, takes a double-click on the chosen one
     *  -- a starting point returning to its values. Returns the y under the row. */
    struct PickerLayout {
        std::vector<juce::Rectangle<float>> cells;  ///< one per name, for a check that clicks one
        float bottom;                               ///< the y under the row
    };
    PickerLayout paintPicker(juce::Graphics& g, float x, float y, float width, const std::vector<juce::String>& names,
                             int selected, bool enabled, const std::function<void(int)>& choose,
                             const std::function<void(int)>& reselect = {}, int unavailable = -1, int columns = 0);

    /// An action, not a value: framed, and inert while disabled.
    void paintButton(juce::Graphics& g, juce::Rectangle<float> area, const juce::String& label, bool enabled,
                     std::function<void()> click);

    /*  One parameter as a tile, drawn and made draggable. `name` overrides the manifest's, for a
     *  page that calls something by a shorter name than the host does. */
    void addParameterTile(juce::Graphics& g, juce::Rectangle<float> tile, ParamId id, const juce::String& name = {},
                          bool enabled = true);

    /// The drag, click and double-click of a host parameter, on an area already drawn.
    void addHostDrag(juce::Rectangle<float> area, ParamId id, bool stepped, bool addsRow = false);

    /*  A drag on a value that is state rather than a host parameter: `span` of it over a whole drag,
     *  in `step`s, held within [`low`, `high`], as one undo step called `name` -- the caller's own
     *  word, shown in the host's undo menu. A click runs `click` when there is one; a double-click,
     *  `reset`. `low` and `high` are open by default, for a value whose setter already holds it in
     *  range; a raw field with no setter passes its own bounds. */
    using StateSet = std::function<void(PluginState&, double)>;
    using StateEdit = std::function<void(PluginState&)>;
    void addStateDrag(juce::Rectangle<float> area, std::string_view name, std::string key, double from, double span,
                      double step, StateSet set, StateEdit reset, std::function<void()> click = {},
                      double low = -std::numeric_limits<double>::infinity(),
                      double high = std::numeric_limits<double>::infinity(), bool wraps = false);

    //  ---- what a plugin answers -----------------------------------------------------------
    /// What the engine is applying for this parameter right now, as against what the patch says: a
    /// value says what you set, a mark says where it is. False when a parameter has no live
    /// counterpart, which is the common case. By default, what the shown engine published.
    virtual bool liveValue(ParamId id, double& out) const { return model_.engineValue(id, out); }

    /// A parameter was double-clicked back to its default. A parameter that something integrates has
    /// more than a value to put back -- an angle under a rate has the turn the rate has added, and a
    /// region angle is handled here, readable from its key. A plugin overriding this for something
    /// else calls the base.
    virtual void onDefaultRestored(ParamId id);
    /// Where the page draws a picture of what plays -- Echo's strip -- repainted when an engine value moves.
    virtual juce::Rectangle<float> enginePicture() const { return {}; }

    /// The LFO's preview as the last paint drew it: the canvas, and the row its grid put zero on. For
    /// the check that the wave and the grid share one convention.
    struct WavePlot {
        juce::Rectangle<float> area;
        std::optional<float> zeroRow;
    };
    WavePlot& wavePlot() { return wave_; }
    const WavePlot& wavePlot() const { return wave_; }

    /// What the last paint found that moves on its own; empty when nothing does. Reset it per paint.
    juce::Rectangle<float>& liveRegion() { return live_; }
    const juce::Rectangle<float>& liveRegion() const { return live_; }
    /*  Repaint what moves on its own, once a frame, from every plugin's tick: what the last paint
     *  found moving -- a source's live output, an angle a rate is turning -- and every tile whose
     *  published value changed since, plus the picture a plugin draws from those values
     *  (`enginePicture`). One request, and none when nothing moves. True when the engine moved
     *  something, so a view drawn from it can repaint too. */
    bool repaintMoving();
    /// How many times something live has been asked to repaint. For checks: a repaint is not observable.
    int liveRepaintsAsked() const { return liveRepaintsAsked_; }

    void clearNameAreas() {
        tileAreas_.fill({});
        tileEnabled_.fill(false);
        namedAreas_.clear();
        pasteDrawn_ = false;
    }

    /// Where something a page drew that is not a parameter's is -- an action's button -- and whether
    /// it takes a press, recorded as a tile's area is so a check clicks where it really is. Empty for
    /// a name the last paint did not draw, or drew disabled.
    void noteNamedArea(const juce::String& name, juce::Rectangle<float> area, bool enabled) {
        namedAreas_.push_back({name, area, enabled});
    }
    juce::Rectangle<float> namedArea(const juce::String& name) const {
        for (const auto& a : namedAreas_)
            if (a.name == name && a.enabled) return a.area;
        return {};
    }
    /// The clipboard is not the patch, so nothing that refreshes the patch notices it change. A page
    /// that drew what the clipboard holds says what it drew, and `followClipboard`, once a frame,
    /// repaints when that is no longer so.
    void notePasteShown(std::string what) {
        pasteShown_ = std::move(what);
        pasteDrawn_ = true;
    }
    void followClipboard();
    /// How often that has asked for a repaint. For checks: a repaint is not observable.
    int clipboardRepaintsAsked() const { return clipboardRepaintsAsked_; }

    bool namedAreaDrawn(const juce::String& name) const {
        for (const auto& a : namedAreas_)
            if (a.name == name) return true;
        return false;
    }

    /// A control a page drew itself -- a strip row's level cell, a sync switch's segments -- records
    /// where it is, exactly as a tile does. Without it a control that is plainly on screen reads as
    /// missing to anything asking what is reachable.
    void noteControl(ParamId id, juce::Rectangle<float> box, bool enabled) {
        if (id == kNoParamId) return;
        tileAreas_[static_cast<std::size_t>(id)] = box;
        tileEnabled_[static_cast<std::size_t>(id)] = enabled;
    }

    /*  The scratch a drag keeps between press and release. Protected because a page builds drags of
     *  its own on values that are neither a host parameter nor a simple piece of state -- a
     *  trajectory's shape parameters are one -- and they must feel identical to the shared ones. */
    std::array<juce::Rectangle<float>, 2> stepAreas_{};
    EnvelopeGraph envelope_;
    float dragStart_{0.0f}, dragY_{0.0f};
    HostDrag hostDrag_;  ///< a host parameter's drag, between its press and its moves
    double stateDragStart_{0.0}, stateDragLast_{0.0};

private:
    PatchModel& model_;
    juce::Rectangle<float> live_;
    std::array<float, kMaxParams> lastEngine_{};  ///< what the engine published at the last frame
    std::array<bool, kMaxParams> hadEngine_{};
    struct NamedArea {
        juce::String name;
        juce::Rectangle<float> area;
        bool enabled{false};
    };
    std::vector<NamedArea> namedAreas_;
    std::string pasteShown_;  ///< what the last paint said the clipboard holds
    bool pasteDrawn_{false};  ///< the last paint drew a paste button at all
    int clipboardRepaintsAsked_{0};
    WavePlot wave_;
    int liveRepaintsAsked_{0};
    std::array<juce::Rectangle<float>, static_cast<std::size_t>(kMaxParams)> tileAreas_{};
    std::array<bool, static_cast<std::size_t>(kMaxParams)> tileEnabled_{};
};

}  // namespace bambi::ui
