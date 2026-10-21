// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/ui/LevelColumn.h"

#include <algorithm>
#include <cmath>

#include "bambi/mod/matrix.hpp"
#include "bambi/ui/Draw.h"
#include "bambi/ui/HostDrag.h"
#include "bambi/ui/Theme.h"

namespace bambi::ui {
namespace {
namespace colour = theme::colour;
namespace mtr = theme::meter;

double normalisedDb(double db) { return std::clamp((db - mtr::floorDb) / -mtr::floorDb, 0.0, 1.0); }
}  // namespace

LevelColumn::LevelColumn(PatchModel& model, std::vector<Level> levels)
    : model_(model), levels_(std::move(levels)), areas_(levels_.size()) {}

void LevelColumn::Meter::feed(float peakLinear, double dtSeconds) {
    const double db = peakLinear > 0.0f ? std::max(mtr::silenceDb, 20.0 * std::log10(static_cast<double>(peakLinear)))
                                        : mtr::silenceDb;
    levelDb = std::max(db, levelDb - mtr::fallDbPerSec * dtSeconds);
    if (db >= holdDb) {
        holdDb = db;
        holdLeft = mtr::holdSeconds;
    } else {
        holdLeft -= dtSeconds;
        if (holdLeft <= 0.0) holdDb -= mtr::fallDbPerSec * dtSeconds;
    }
}

void LevelColumn::update(float peakLinear, double dtSeconds) {
    channels_ = 0;
    meters_.resize(1);
    meters_[0].feed(peakLinear, dtSeconds);
    repaint();
}

void LevelColumn::updateChannels(const float* peaksLinear, int channels, double dtSeconds) {
    if (channels <= 0) return update(0.0f, dtSeconds);
    if (channels != channels_) meters_.assign(static_cast<std::size_t>(channels), Meter{});  // a new bus: fresh bars
    channels_ = channels;
    for (int c = 0; c < channels; ++c) meters_[static_cast<std::size_t>(c)].feed(peaksLinear[c], dtSeconds);
    repaint();
}

juce::Rectangle<float> LevelColumn::valueArea(ParamId id) const {
    for (std::size_t i = 0; i < levels_.size(); ++i)
        if (levels_[i].id == id) return areas_[i];
    return {};
}

void LevelColumn::mouseMove(const juce::MouseEvent& e) {
    const bool over = std::any_of(areas_.begin(), areas_.end(),
                                  [&](const juce::Rectangle<float>& a) { return a.contains(e.position); });
    setMouseCursor(over ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
}

void LevelColumn::paint(juce::Graphics& g) {
    clearRegions();
    const auto width = static_cast<float>(getWidth());

    //  the levels, one under another: the number, and under it what it is
    const auto blockHeight = theme::type::meterValue + theme::type::small + mtr::valueLabelGap;
    auto y = mtr::valueTop;
    for (std::size_t i = 0; i < levels_.size(); ++i) {
        const auto id = levels_[i].id;
        const juce::Rectangle<float> block{0.0f, y, width, blockHeight};
        areas_[i] = block;
        const auto db = static_cast<double>(model_.valueOf(id));
        text(g, signedNumber(db, 1), font(theme::type::meterValue), colour::text,
             block.withHeight(theme::type::meterValue + mtr::valueLabelGap), juce::Justification::centred);
        //  named in the ink a tile's name takes while something drives it or its row is open
        const bool driven = model_.committed(id) || model_.provisionalRow() == id;
        text(g, levels_[i].name + " db", smallFont(), driven ? colour::text : colour::label,
             {0.0f, y + theme::type::meterValue + mtr::valueLabelGap, width, theme::type::small},
             juce::Justification::centred);

        //  moved exactly as a tile's value is: one statement of it, not a second
        addHostDrag(*this, model_, drag_, block, id, false, isModulationTarget(model_.modManifest(), id));
        y += blockHeight + mtr::unitGap;
    }

    //  the output, under them: one bar, or a bar per channel of the bus across the same well
    const auto bar = static_cast<float>(theme::metrics::meterBar);
    const int bars = std::max(1, channels_);
    float barWidth = bar, gap = 0.0f;
    if (bars == 2) {
        gap = mtr::pairGap;
    } else if (bars > 2) {
        gap = bars >= mtr::denseFrom ? 0.0f : mtr::channelGap;
        barWidth = std::max(
            0.5f, (width - 2.0f * mtr::channelInset - gap * static_cast<float>(bars - 1)) / static_cast<float>(bars));
    }
    const float span = barWidth * static_cast<float>(bars) + gap * static_cast<float>(bars - 1);
    const juce::Rectangle<float> well{(width - span) / 2.0f, y, span,
                                      static_cast<float>(getHeight()) - y - mtr::footer};
    g.setColour(colour::panel);
    g.fillRect(well);
    const auto yFor = [&](double db) {
        return well.getBottom() - static_cast<float>(normalisedDb(db)) * well.getHeight();
    };
    for (int b = 0; b < bars; ++b) {
        const Meter& m = meters_[static_cast<std::size_t>(b)];
        const float x = well.getX() + static_cast<float>(b) * (barWidth + gap);
        if (gap > 0.0f && b > 0) {
            g.setColour(colour::ground);
            g.fillRect(x - gap, well.getY(), gap, well.getHeight());
        }
        const auto levelTop = yFor(m.levelDb);
        g.setColour(colour::meterFill);
        g.fillRect(x, levelTop, barWidth, well.getBottom() - levelTop);
        if (m.holdDb > mtr::floorDb) {
            nowMark(g, {x, yFor(m.holdDb) - theme::stroke::meterPeak / 2.0f, barWidth, theme::stroke::meterPeak},
                    colour::meterPeak);
        }
    }
    for (const double tick : mtr::ticksDb) ruleH(g, well.getX(), well.getRight(), yFor(tick), colour::meterTick);
    text(g, channels_ > 0 ? juce::String(channels_) + " ch" : juce::String("out"), smallFont(), colour::label,
         {0.0f, static_cast<float>(getHeight()) - mtr::footer, width, mtr::footer}, juce::Justification::centred);
}

}  // namespace bambi::ui
