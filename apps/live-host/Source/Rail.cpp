// SPDX-License-Identifier: GPL-3.0-or-later
#include "Rail.h"

#include <algorithm>
#include <cmath>

#include "bambi/ui/Draw.h"
#include "bambi/ui/ProductName.h"
#include "bambi/ui/Widgets.h"

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

void Rail::setPlaying(bool playing) {
    if (playing == playing_) return;
    playing_ = playing;
    repaint();
}

void Rail::setStatus(Status status) {
    const bool same = status.on == status_.on && status.lines == status_.lines && status.problem == status_.problem;
    status_ = std::move(status);
    if (!same) repaint();
}

int Rail::width() {
    float widest = 0.0f;
    for (const Product p : ui::kProducts) widest = std::max(widest, ui::textWidth(ui::labelFont(), ui::productName(p)));
    for (const char* page : {"I/O", "routing", "timeline"})
        widest = std::max(widest, ui::textWidth(ui::labelFont(), page));
    return static_cast<int>(std::ceil(widest + 2.0f * rail::padX + ui::theme::stroke::rule));
}

void Rail::paint(juce::Graphics& g) {
    const auto w = static_cast<float>(getWidth());
    const auto h = static_cast<float>(getHeight());
    const auto headerBottom = static_cast<float>(ui::theme::metrics::headerHeight);
    g.fillAll(colour::ground);
    //  the plugin's header line, carried across; the plugin's rule ends where the column begins
    ui::ruleH(g, 0.0f, w, headerBottom - ui::theme::stroke::rule, colour::rule);
    ui::ruleV(g, w - ui::theme::stroke::rule, 0.0f, h, colour::ruleStrong);

    //  Live's transport in the header's height: play, then stop
    const float glyph = rail::transport, mid = headerBottom / 2.0f;
    play_ = {rail::padX, mid - glyph / 2.0f, glyph, glyph};
    stop_ = play_.translated(glyph + rail::transportGap, 0.0f);
    ui::drawPlayGlyph(g, play_, playing_);
    ui::drawStopGlyph(g, stop_);

    //  the status from the foot up: its first line beside the square
    const auto square = ui::theme::header::statusSquare;
    const float statusTop = h - rail::padX - rail::row * static_cast<float>(status_.lines.size());

    //  the views: up to the first empty label from the top, the rest standing on the status
    areas_.assign(labels_.size(), {});
    const auto split =
        static_cast<std::size_t>(std::find(labels_.begin(), labels_.end(), juce::String()) - labels_.begin());
    const auto view = [&](std::size_t i, float y) {
        const juce::Rectangle<float> row{0.0f, y, w - ui::theme::stroke::rule, rail::row};
        ui::switchLabel(g, labels_[i], row.withTrimmedLeft(rail::padX), row, static_cast<int>(i) == open_);
        areas_[i] = row;
    };
    float top = headerBottom + rail::top;
    for (std::size_t i = 0; i < split; ++i, top += rail::row) view(i, top);
    std::size_t below = 0;
    for (std::size_t i = split; i < labels_.size(); ++i) below += labels_[i].isEmpty() ? 0 : 1;
    float foot = statusTop - rail::gap - rail::row * static_cast<float>(below);
    const float footTop = foot;
    for (std::size_t i = split; i < labels_.size(); ++i)
        if (labels_[i].isNotEmpty()) {
            view(i, foot);
            foot += rail::row;
        }

    //  what went wrong, between the two groups
    if (status_.problem.isNotEmpty()) {
        const juce::Rectangle<float> box{rail::padX, top + rail::gap, w - 2.0f * rail::padX,
                                         footTop - rail::gap - top - rail::gap};
        g.setColour(colour::text);
        g.setFont(ui::smallFont());
        g.drawFittedText(status_.problem, box.toNearestInt(), juce::Justification::bottomLeft, rail::problemLines,
                         1.0f);
    }
    float line = statusTop;
    for (std::size_t i = 0; i < status_.lines.size(); ++i) {
        auto box = juce::Rectangle<float>{rail::padX, line, w - 2.0f * rail::padX, rail::row};
        if (i == 0) {
            ui::statusSquare(g, box.getX(), box.getCentreY(), status_.on ? colour::ok : colour::inactive);
            box = box.withTrimmedLeft(square + rail::padX / 2.0f);
        }
        ui::text(g, status_.lines[i], ui::smallFont(), i == 0 ? colour::text : colour::label, box);
        line += rail::row;
    }
}

bool Rail::takeKey(const juce::KeyPress& key) {
    if (key != juce::KeyPress::spaceKey || !transport) return false;
    transport(!playing_);
    return true;
}

void Rail::pressAt(juce::Point<float> at) {
    if (transport && play_.expanded(rail::transportGap / 2.0f).contains(at)) return transport(true);
    if (transport && stop_.expanded(rail::transportGap / 2.0f).contains(at)) return transport(false);
    for (std::size_t i = 0; i < areas_.size(); ++i)
        if (areas_[i].contains(at) && select_) return select_(static_cast<int>(i));
}

void Rail::mouseDown(const juce::MouseEvent& e) { pressAt(e.position); }

}  // namespace bambi::livehost
