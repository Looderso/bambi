// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "bambi/editor/PatchControls.h"
#include "bambi/host/PluginProcessor.h"
#include "bambi/ui/PresetModel.h"

namespace bambi::editor {

/*  What the header and the browser ask about presets, answered from the shared processor base:
    `ui` knows no processor and `host` draws nothing, and this is where they meet. This window's own
    instance, always -- while the controls are on another one nothing is loaded or saved, as the
    energy picture is not drawn. Rename and remove are the file's, which is the product's and not
    any instance's. */
class ProcessorPresets final : public ui::PresetModel {
public:
    ProcessorPresets(host::PluginProcessor& processor, const PatchControls& controls)
        : processor_(processor), controls_(controls) {}

    bool available() const override { return !controls_.remote; }
    void refresh() override { processor_.refreshPresets(); }

    const std::vector<PresetRef>& all() override { return processor_.presets().all(); }
    std::vector<std::string> userFolders() override { return processor_.presets().folders(false); }
    PresetRef current() const override { return processor_.currentPreset(); }
    bool modified() override { return processor_.presetModified(); }

    bool load(const PresetRef& ref) override { return available() && processor_.loadPreset(ref); }
    bool step(int delta) override { return available() && processor_.stepPreset(delta); }
    bool save(const PresetRef& ref) override { return available() && processor_.savePreset(ref); }
    bool rename(const PresetRef& from, const PresetRef& to) override { return processor_.renamePreset(from, to); }
    bool remove(const PresetRef& ref) override { return processor_.removePreset(ref); }

    bool folded(std::string_view heading) override { return processor_.presets().folded(heading); }
    void setFolded(std::string_view heading, bool folded) override { processor_.presets().setFolded(heading, folded); }

private:
    host::PluginProcessor& processor_;
    const PatchControls& controls_;
};

}  // namespace bambi::editor
