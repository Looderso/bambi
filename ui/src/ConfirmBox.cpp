// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/ui/ConfirmBox.h"

#include "bambi/ui/Draw.h"
#include "bambi/ui/Theme.h"
#include "bambi/ui/Widgets.h"

namespace bambi::ui {
namespace {
namespace colour = theme::colour;
namespace cf = theme::confirm;
namespace ctl = theme::controls;
}  // namespace

ConfirmBox::ConfirmBox() = default;

void ConfirmBox::ask(juce::String question, juce::String note, juce::String yes, std::function<void()> confirmed) {
    question_ = std::move(question);
    note_ = std::move(note);
    yesLabel_ = std::move(yes);
    confirmed_ = std::move(confirmed);
    open();  // return and escape answer it
}

void ConfirmBox::answer(bool yes) {
    if (!isOpen()) return;
    close();
    auto confirmed = std::move(confirmed_);
    confirmed_ = nullptr;
    if (yes && confirmed) confirmed();
    if (answered) answered();
}

bool ConfirmBox::keyPressed(const juce::KeyPress& key) {
    if (key == juce::KeyPress::returnKey) {
        answer(true);
        return true;
    }
    return Sheet::keyPressed(key);
}

void ConfirmBox::paint(juce::Graphics& g) {
    const auto height = 2.0f * cf::pad + 2.0f * cf::line + ctl::sectionGap + ctl::segmentHeight;
    const auto box = juce::Rectangle<float>(cf::width, height).withCentre(getLocalBounds().toFloat().getCentre());
    paintSheet(g, box);

    auto inner = box.reduced(cf::pad);
    text(g, question_, font(theme::type::body), colour::text, inner.removeFromTop(cf::line));
    text(g, note_, labelFont(), colour::label, inner.removeFromTop(cf::line));

    auto buttons = inner.removeFromBottom(ctl::segmentHeight);
    //  cancel on the left, what was asked for on the right: the order every row of two here has
    yes_ = buttons.removeFromRight(framedButtonWidth(yesLabel_));
    buttons.removeFromRight(theme::settings::buttonGap);
    no_ = buttons.removeFromRight(framedButtonWidth("cancel"));
    drawFramedButton(g, yes_, yesLabel_, true);
    drawFramedButton(g, no_, "cancel", true);
    addClickArea(yes_, [this] { answer(true); });
    addClickArea(no_, [this] { answer(false); });
}

}  // namespace bambi::ui
