// SPDX-License-Identifier: GPL-3.0-or-later
#include "PluginEditor.h"

namespace {
/*  NOT called `id`: inside this class's constructor a bare name is looked up in the class first,
    and a base's member of that name would be called on a base that is not built yet. */
bambi::ParamId reverbParam(const char* key) { return static_cast<bambi::ParamId>(bambi::reverbParams().byKey(key)); }
}  // namespace

BambiReverbEditor::BambiReverbEditor(BambiReverbProcessor& p)
    : bambi::ui::ReverbWindowParts(p),
      bambi::editor::EffectEditor(p, controls, panel, "reverb",
                                  {{reverbParam("output.dry"), "dry"}, {reverbParam("output.wet"), "wet"}},
                                  {{reverbParam("rates.retrigger"), "rates continue"}}),
      processor_(p) {}

void BambiReverbEditor::addEffectSettings(bambi::ui::SettingsContent& c) {
    /*  What a bounce should do is state, tied to this instance, as the whole page is -- so while the
        controls show another one it is inert: the header's undo acts on the instance shown, and
        would take back the wrong document. */
    /*  The quality while playing is a host parameter, so it goes through the controls and follows
        the instance they show. */
    const auto playing = reverbParam("quality.playing");
    c.own.push_back({"while playing",
                     {"efficient", "realistic"},
                     controls.valueOf(playing) > 0.5f ? 1 : 0,
                     [this, playing](int which) {
                         controls.changeParameter(playing, which > 0 ? 1.0f : 0.0f);
                         controls.notifyChanged();
                     },
                     "realistic costs little more: the line count is the cost, not the tail's order",
                     true});
    c.own.push_back({"when rendering",
                     {"same as playing", "realistic"},
                     processor_.renderQuality() == bambi::RenderQuality::Realistic ? 1 : 0,
                     [this](int which) {
                         processor_.setRenderQuality(which > 0 ? bambi::RenderQuality::Realistic
                                                               : bambi::RenderQuality::Same);
                     },
                     controls.remote ? "select this window's own instance to change it"
                                     : "a render may take the better setting while playback stays light",
                     !controls.remote});
}

void BambiReverbEditor::showSlot(int which) {
    panel.showSlot(which);
    panel.repaint();
}

bool BambiReverbEditor::clickPreset(int index) {
    paintNow();
    if (panel.scrollIntoView(panel.roomButton(index)))  // as a tile is: what is hidden takes no press
        paintNow();
    const auto box = panel.roomButton(index);
    if (box.isEmpty()) return false;
    panel.clickAt(box.getCentre());
    return true;
}
