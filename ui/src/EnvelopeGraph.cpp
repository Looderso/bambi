// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/ui/EnvelopeGraph.h"

#include <algorithm>
#include <array>

#include "bambi/ui/Draw.h"
#include "bambi/ui/ParameterPage.h"
#include "bambi/ui/Tiles.h"
#include "bambi/ui/Widgets.h"

namespace bambi::ui {
namespace {
namespace colour = theme::colour;
namespace ctl = theme::controls;

/// Which parameters a grab moves: the decay corner carries the sustain with it, as the picture shows.
std::array<ParamId, 2> movedBy(const EnvelopeGraph::Ids& ids, EnvelopeGrab grab) {
    switch (grab) {
        case EnvelopeGrab::Attack: return {ids.attack, kNoParamId};
        case EnvelopeGrab::DecaySustain: return {ids.decay, ids.sustain};
        case EnvelopeGrab::SustainEdge: return {ids.sustain, kNoParamId};
        case EnvelopeGrab::Release: return {ids.release, kNoParamId};
        case EnvelopeGrab::AttackCurve: return {ids.attackCurve, kNoParamId};
        case EnvelopeGrab::DecayCurve: return {ids.decayCurve, kNoParamId};
        case EnvelopeGrab::ReleaseCurve: return {ids.releaseCurve, kNoParamId};
        case EnvelopeGrab::None: break;
    }
    return {kNoParamId, kNoParamId};
}

/// A diamond: a square on its corner, which is what says "this sets a shape" and not "this sets a value".
void diamond(juce::Graphics& g, juce::Point<float> at, float half) {
    juce::Path path;
    path.startNewSubPath(at.x, at.y - half);
    path.lineTo(at.x + half, at.y);
    path.lineTo(at.x, at.y + half);
    path.lineTo(at.x - half, at.y);
    path.closeSubPath();
    g.fillPath(path);
}

}  // namespace

float EnvelopeGraph::paint(ParameterPage& page, juce::Graphics& g, juce::Rectangle<float> content, float y,
                           const Ids& ids, double polarity) {
    auto& model = page.model();
    const auto& manifest = model.manifest();
    const auto of = [&model](ParamId at) { return static_cast<double>(model.valueOf(at)); };

    const EnvelopeShape env{of(ids.attack),      of(ids.decay),      of(ids.sustain),     of(ids.release),
                            of(ids.attackCurve), of(ids.decayCurve), of(ids.releaseCurve)};

    //  frozen while something is held: a plot that grows to keep an extreme drag on screen would move
    //  every other point out from under the cursor
    if (held_ == EnvelopeGrab::None) span_ = envelopeSpan(env);
    const auto span = std::max(span_, 1e-9);

    const juce::Rectangle<float> box{content.getX(), y, content.getWidth(), ctl::envelopeHeight};
    const juce::Rectangle<float> plot{box.getX() + ctl::envelopeGutter, box.getY(),
                                      box.getWidth() - ctl::envelopeGutter, box.getHeight()};
    const auto scale = plot.getWidth() / static_cast<float>(span);
    const auto atX = [&](double units) { return plot.getX() + scale * static_cast<float>(units); };
    const auto atY = [&](double v) { return plot.getBottom() - plot.getHeight() * static_cast<float>(v); };

    //  the axis both source graphs share: polarity relabels it and moves nothing drawn
    drawNiceGrid(g, plot, polarity, ctl::envelopeTicks);

    const auto stops = envelopeStops(env);
    g.setColour(colour::envelopeGrid);
    for (std::size_t i = 1; i < stops.size(); ++i)
        g.drawVerticalLine(juce::roundToInt(atX(stops[i])), plot.getY(), plot.getBottom());

    //  the axis itself, where the picture starts and where it ends
    g.setColour(colour::rule);
    g.drawVerticalLine(juce::roundToInt(plot.getX()), plot.getY(), plot.getBottom());
    ruleH(g, plot.getX(), plot.getRight(), plot.getBottom(), colour::rule);

    //  the hold, shaded across the whole plot: a PREVIEW convention, drawn unlike any timed stage
    if (stops[3] > stops[2])
        g.setColour(colour::output().withAlpha(theme::scene::regionWashAlpha)),
            g.fillRect(
                juce::Rectangle<float>{atX(stops[2]), plot.getY(), atX(stops[3]) - atX(stops[2]), plot.getHeight()});

    g.setColour(colour::output());
    juce::Path path;
    bool first = true;
    for (const auto& p : envelopeStageCurve(env, ctl::envelopeCurvePoints)) {
        const auto px = atX(p.x), py = atY(p.y);
        if (first)
            path.startNewSubPath(px, py);
        else
            path.lineTo(px, py);
        first = false;
    }
    g.strokePath(path, juce::PathStrokeType(theme::stroke::curve));

    plot_ = plot;
    const auto handle = [&](EnvelopeGrab grab) {
        const auto at = envelopeGrabPoint(env, grab);
        const juce::Point<float> where{atX(at.x), atY(at.y)};
        handles_[index(grab)] = where;
        return where;
    };
    //  the curve handles use the same handle role a trajectory node's do, already violet; the
    //  curve itself is `output`, the same role as the bar above it
    g.setColour(colour::handle);
    for (const auto grab : {EnvelopeGrab::AttackCurve, EnvelopeGrab::DecayCurve, EnvelopeGrab::ReleaseCurve})
        diamond(g, handle(grab), ctl::envelopeHandle);
    for (const auto grab :
         {EnvelopeGrab::Attack, EnvelopeGrab::DecaySustain, EnvelopeGrab::SustainEdge, EnvelopeGrab::Release}) {
        const auto at = handle(grab);
        g.setColour(grab == held_ ? colour::handle : colour::text);
        g.fillEllipse(at.x - ctl::envelopePoint, at.y - ctl::envelopePoint, 2.0f * ctl::envelopePoint,
                      2.0f * ctl::envelopePoint);
    }
    //  every handle takes a drag, captured by value: a gesture outlives the paint that registered
    //  it, and a lambda holding this paint's rectangle by reference reads a dead local on the first drag
    const auto units = [plot, scale](float px) { return static_cast<double>((px - plot.getX()) / scale); };
    const auto level = [plot](float px) { return static_cast<double>((plot.getBottom() - px) / plot.getHeight()); };
    const auto set = [&model, &manifest](ParamId at, double value) {
        model.setParameter(at, static_cast<float>(normalised(manifest, at, value)));
    };

    /*  A little wider than the plot: the release point sits exactly on the bottom edge, and a
        rectangle does not contain its own bottom -- so the one handle at the end of the shape took
        no press at all. Grown by a point's own radius, which is well inside the gap to the tiles. */
    page.addDragArea(
        plot.expanded(ctl::envelopePoint),
        [this, &model, env, ids, units, level, scale, plot](juce::Point<float> p) {
            span_ = envelopeSpan(env);
            held_ = envelopeGrabAt(env, units(p.x), level(p.y), static_cast<double>(ctl::envelopeGrab / scale),
                                   static_cast<double>(ctl::envelopeLineGrab / plot.getHeight()));
            lastY_ = p.y;
            for (const auto at : movedBy(ids, held_))
                if (at != kNoParamId) model.beginParameter(at);
        },
        [this, &model, &manifest, env, ids, units, set, plot, scale](juce::Point<float> p, bool fine) {
            if (held_ == EnvelopeGrab::None) return;
            const auto dy = (p.y - lastY_) * (fine ? ctl::fineDrag : 1.0f);
            lastY_ = p.y;

            /*  A time is solved from where the cursor is, absolutely, every move -- not carried
                along by a delta, which is what lets a point drift out from under it. Held inside
                the plot's own visible width, so a drag sticks to the edge rather than solving a
                valid value that renders off it. */
            const auto onTime = [&](ParamId at) {
                const auto& d = manifest[static_cast<int>(at)];
                const auto x = std::clamp(units(p.x), 0.0, static_cast<double>(plot.getWidth() / scale));
                set(at, envelopeStageMs(env, held_, x, static_cast<double>(d.min), static_cast<double>(d.max)));
            };
            const auto onLevel = [&] {
                set(ids.sustain, std::clamp(static_cast<double>(model.valueOf(ids.sustain)) -
                                                static_cast<double>(dy / plot.getHeight()),
                                            0.0, 1.0));
            };
            switch (held_) {
                case EnvelopeGrab::Attack: onTime(ids.attack); break;
                case EnvelopeGrab::DecaySustain:
                    onTime(ids.decay);
                    onLevel();
                    break;
                case EnvelopeGrab::SustainEdge: onLevel(); break;  // its width is a preview, not a parameter
                case EnvelopeGrab::Release: onTime(ids.release); break;
                default: {
                    const auto at = movedBy(ids, held_)[0];
                    set(at, envelopeBow(static_cast<double>(model.valueOf(at)), held_, static_cast<double>(dy),
                                        static_cast<double>(ctl::envelopeBowPixels)));
                    break;
                }
            }
            model.notifyChanged();
        },
        [this, &model, ids] {
            for (const auto at : movedBy(ids, held_))
                if (at != kNoParamId) model.endParameter(at);
            held_ = EnvelopeGrab::None;  // and the scale settles again on the next paint
        });

    return box.getBottom() + ctl::curveGap;
}

}  // namespace bambi::ui
