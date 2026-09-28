// SPDX-License-Identifier: GPL-3.0-or-later
#include "PluginEditor.h"

namespace {
bambi::ParamId echoParam(const char* key) { return static_cast<bambi::ParamId>(bambi::echoParams().byKey(key)); }
}  // namespace

BambiEchoEditor::BambiEchoEditor(BambiEchoProcessor& p)
    : bambi::ui::EchoWindowParts(p),
      bambi::editor::EffectEditor(p, controls, panel, "echo",
                                  {{echoParam("output.dry"), "dry"}, {echoParam("output.wet"), "wet"}},
                                  {{echoParam("rates.retrigger"), "rates continue"}}) {}

void BambiEchoEditor::selectTap(int index) {
    panel.selectTap(index);
    panel.repaint();
}

void BambiEchoEditor::showCategory(int index) {
    panel.showCategory(index);
    panel.repaint();
}

bool BambiEchoEditor::clickTapRow(int index) {
    paintNow();
    const auto row = panel.tapRow(index);
    if (row.isEmpty()) return false;
    //  The right-hand end of the row is its level; the left is its switch. The middle is the row.
    panel.clickAt({row.getX() + 2.0f, row.getCentreY()});
    return true;
}

bool BambiEchoEditor::clickTapSwitch(int index) {
    paintNow();
    const auto box = panel.tapSwitch(index);
    if (box.isEmpty()) return false;
    panel.clickAt(box.getCentre());
    return true;
}

bool BambiEchoEditor::drawnOnPanel(bambi::ParamId id) {
    for (int i = 0; i < bambi::kEchoTaps; ++i)
        if (id == controls.tapParam(i, "on") && !panel.tapSwitch(i).isEmpty()) return true;
    return false;
}
