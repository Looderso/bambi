// SPDX-License-Identifier: GPL-3.0-or-later
#include "Rail.h"

#include "bambi/ui/Draw.h"

namespace bambi::livehost {

namespace colour = ui::theme::colour;
namespace rail = ui::theme::rail;

void Rail::setViews(std::vector<juce::String> labels, int open, std::function<void(int)> select) {
    const bool same = labels == labels_ && open == open_;
    labels_ = std::move(labels);
    open_ = open;
    select_ = std::move(select);
    if (!same) repaint();
}

void Rail::setStatus(Status status) {
    const bool same = status.on == status_.on && status.lines == status_.lines && status.problem == status_.problem;
    status_ = std::move(status);
    if (!same) repaint();
}

void Rail::paint(juce::Graphics& g) {
    const auto w = static_cast<float>(getWidth());
    const auto h = static_cast<float>(getHeight());
    const auto headerBottom = static_cast<float>(ui::theme::metrics::headerHeight);
    g.fillAll(colour::ground);
    //  the plugin's header line, carried across; the plugin's rule ends where the column begins
    ui::ruleH(g, 0.0f, w, headerBottom - ui::theme::stroke::rule, colour::rule);
    ui::ruleV(g, w - ui::theme::stroke::rule, 0.0f, h, colour::ruleStrong);

    areas_.assign(labels_.size(), {});
    auto y = headerBottom + rail::top;
    for (std::size_t i = 0; i < labels_.size(); ++i) {
        if (labels_[i].isEmpty()) {
            y += rail::gap;
            continue;
        }
        const juce::Rectangle<float> row{0.0f, y, w - ui::theme::stroke::rule, rail::row};
        ui::switchLabel(g, labels_[i], row.withTrimmedLeft(rail::padX), row, static_cast<int>(i) == open_);
        areas_[i] = row;
        y += rail::row;
    }

    //  the status from the foot up: its first line beside the square
    const auto square = ui::theme::header::statusSquare;
    auto line = h - rail::padX - rail::row * static_cast<float>(status_.lines.size());
    if (status_.problem.isNotEmpty()) {
        const auto box =
            juce::Rectangle<float>{rail::padX, headerBottom, w - 2.0f * rail::padX, line - headerBottom}.withTrimmedTop(
                rail::top + rail::row * static_cast<float>(labels_.size()));
        g.setColour(colour::text);
        g.setFont(ui::smallFont());
        g.drawFittedText(status_.problem, box.toNearestInt(), juce::Justification::bottomLeft, rail::problemLines,
                         1.0f);
    }
    for (std::size_t i = 0; i < status_.lines.size(); ++i) {
        auto box = juce::Rectangle<float>{rail::padX, line, w - 2.0f * rail::padX, rail::row};
        if (i == 0) {
            g.setColour(status_.on ? colour::ok : colour::inactive);
            g.fillRect(box.getX(), box.getCentreY() - square / 2.0f, square, square);
            box = box.withTrimmedLeft(square + rail::padX / 2.0f);
        }
        ui::text(g, status_.lines[i], ui::smallFont(), i == 0 ? colour::text : colour::label, box);
        line += rail::row;
    }
}

void Rail::mouseDown(const juce::MouseEvent& e) {
    for (std::size_t i = 0; i < areas_.size(); ++i)
        if (areas_[i].contains(e.position) && select_) return select_(static_cast<int>(i));
}

}  // namespace bambi::livehost
