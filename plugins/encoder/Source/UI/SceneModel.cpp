// SPDX-License-Identifier: GPL-3.0-or-later
#include "UI/SceneModel.h"

#include "bambi/encode/params.hpp"

namespace bambi::ui {

const SceneInstance* SceneState::find(const bambi::Uuid& id) const {
    if (id.isNil()) return nullptr;
    for (const auto& instance : instances)
        if (instance.id == id) return &instance;
    return nullptr;
}

void SceneState::rebuild(const bambi::LinkScene& scene, const bambi::Uuid& self, bambi::Product product) {
    const auto& entries = scene.entries();

    /*  This plugin's kind only. One bus carries all three, and this list is what the
        window can select and edit: a parameter position means a different parameter in a different
        plugin, so an Echo listed in an encoder's header offers an edit that cannot be made. An
        effect has no source to draw either. */
    std::size_t kept = 0;
    for (const auto& entry : entries)
        if (entry.product == product) ++kept;

    instances.resize(kept);  // paths keep their storage from frame to frame
    std::size_t i = 0;
    for (const auto& entry : entries) {
        if (entry.product != product) continue;
        auto& instance = instances[i++];
        instance.id = entry.instance;
        instance.self = entry.instance == self;
        instance.hasPosition = entry.hasPosition;
        instance.position = entry.dyn.position();
        instance.s = static_cast<double>(entry.dyn.s);
        instance.inputMode = entry.dyn.inputMode;
        instance.left = entry.dyn.left();
        instance.right = entry.dyn.right();
        //  width as rendered, modulation included: the engine's value, from the record every window reads
        const auto width = static_cast<std::uint32_t>(bambi::EncoderParam::RenderWidth);
        instance.widthRad =
            width < entry.dyn.liveCount ? static_cast<double>(entry.dyn.live[width]) * bambi::kDeg2Rad : 0.0;

        instance.label = juce::String(entry.label());
        instance.closed = entry.st.closed != 0;
        instance.centre = entry.st.centreVec();
        instance.transform = entry.dyn.transform();
        if (entry.generation > 0)
            bambi::resolvePath(entry.st, entry.dyn, instance.path);
        else
            instance.path.clear();
    }

    if (find(selected) == nullptr) selected = self;
    if (find(hovered) == nullptr) hovered = {};
}

void SceneState::say(const juce::String& text) {
    notice = text;
    noticeUntilMs = juce::Time::getMillisecondCounterHiRes() + 2000.0;
    if (changed) changed();
}

void SceneState::select(const bambi::Uuid& id) {
    if (id == selected) return;
    selected = id;
    if (userAction) userAction();
    if (changed) changed();
}

}  // namespace bambi::ui
