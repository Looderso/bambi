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

void LevelColumn::update(float peakLinear, double dtSeconds) {
    const double db = peakLinear > 0.0f ? std::max(mtr::silenceDb, 20.0 * std::log10(static_cast<double>(peakLinear)))
                                        : mtr::silenceDb;
    levelDb_ = std::max(db, levelDb_ - mtr::fallDbPerSec * dtSeconds);
    if (db >= holdDb_) {
        holdDb_ = db;
        holdLeft_ = mtr::holdSeconds;
    } else if ((holdLeft_ -= dtSeconds) <= 0.0) {
        holdDb_ -= mtr::fallDbPerSec * dtSeconds;
    }
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

    //  the output, under them
    const auto bar = static_cast<float>(theme::metrics::meterBar);
    const juce::Rectangle<float> well{(width - bar) / 2.0f, y, bar, static_cast<float>(getHeight()) - y - mtr::footer};
    g.setColour(colour::panel);
    g.fillRect(well);
    const auto yFor = [&](double db) {
        return well.getBottom() - static_cast<float>(normalisedDb(db)) * well.getHeight();
    };

    const auto levelTop = yFor(levelDb_);
    g.setColour(colour::meterFill);
    g.fillRect(well.getX(), levelTop, well.getWidth(), well.getBottom() - levelTop);
    for (const double tick : mtr::ticksDb) ruleH(g, well.getX(), well.getRight(), yFor(tick), colour::meterTick);
    if (holdDb_ > mtr::floorDb) {
        g.setColour(colour::meterPeak);
        g.fillRect(well.getX(), yFor(holdDb_) - theme::stroke::meterPeak / 2.0f, well.getWidth(),
                   theme::stroke::meterPeak);
    }
    text(g, "out", smallFont(), colour::label,
         {0.0f, static_cast<float>(getHeight()) - mtr::footer, width, mtr::footer}, juce::Justification::centred);
}

}  // namespace bambi::ui
