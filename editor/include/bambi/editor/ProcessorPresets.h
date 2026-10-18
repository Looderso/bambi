// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "bambi/editor/PatchControls.h"
#include "bambi/host/PluginProcessor.h"
#include "bambi/ui/PresetModel.h"

namespace bambi::editor {

/*  What the header and the browser ask about presets, answered from the shared processor base:
    `ui` knows no processor and `host` draws nothing, and this is where they meet. The window's shown
    instance: this one, whose processor loads and saves, or another in the session, which is sent the whole
    state of the preset to apply as one undo step of its own and is saved from the state the window shows.
    The library and the files are the product's either way; rename and remove touch only the file. */
class ProcessorPresets final : public ui::PresetModel {
public:
    ProcessorPresets(host::PluginProcessor& processor, PatchControls& controls)
        : processor_(processor), controls_(controls) {}

    bool available() const override { return !controls_.remote || controls_.remoteTarget.ready(); }
    void refresh() override { processor_.refreshPresets(); }

    const std::vector<PresetRef>& all() override { return processor_.presets().all(); }
    std::vector<std::string> userFolders() override { return processor_.presets().folders(false); }
    PresetRef current() const override {
        return controls_.remote ? controls_.state().preset : processor_.currentPreset();
    }
    bool modified() override {
        return controls_.remote ? processor_.presetModified(controls_.state()) : processor_.presetModified();
    }

    bool load(const PresetRef& ref) override {
        if (!available()) return false;
        if (!controls_.remote) return processor_.loadPreset(ref);
        const auto next = processor_.presetOver(ref, controls_.remoteTarget.state());
        return next.has_value() && controls_.remoteTarget.loadWhole("load preset", *next);
    }
    bool step(int delta) override {
        if (!available()) return false;
        if (!controls_.remote) return processor_.stepPreset(delta);
        return load(bambi::stepPreset(processor_.presets().all(), current(), delta));
    }
    bool save(const PresetRef& ref) override {
        if (!available()) return false;
        if (!controls_.remote) return processor_.savePreset(ref);
        return processor_.presets().save(ref, controls_.state()) && controls_.remoteTarget.setPreset(ref);
    }
    bool rename(const PresetRef& from, const PresetRef& to) override { return processor_.renamePreset(from, to); }
    bool remove(const PresetRef& ref) override { return processor_.removePreset(ref); }

    bool folded(std::string_view heading) override { return processor_.presets().folded(heading); }
    void setFolded(std::string_view heading, bool folded) override { processor_.presets().setFolded(heading, folded); }

private:
    host::PluginProcessor& processor_;
    PatchControls& controls_;
};

}  // namespace bambi::editor
