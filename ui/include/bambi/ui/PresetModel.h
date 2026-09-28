// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "bambi/patch/presets.hpp"

namespace bambi::ui {

/*  What the header's preset cluster and the browser ask of whoever owns the presets -- a processor,
    which `ui` does not know. One interface for every plugin; `editor` answers it from the shared
    processor base. */
class PresetModel {
public:
    virtual ~PresetModel() = default;

    /// False while the window shows another instance: a preset is this instance's own.
    virtual bool available() const = 0;
    /// Read the disk again: the browser is opening.
    virtual void refresh() = 0;

    virtual const std::vector<PresetRef>& all() = 0;
    virtual std::vector<std::string> userFolders() = 0;
    virtual PresetRef current() const = 0;
    virtual bool modified() = 0;

    virtual bool load(const PresetRef& ref) = 0;
    virtual bool step(int delta) = 0;
    virtual bool save(const PresetRef& ref) = 0;
    virtual bool rename(const PresetRef& from, const PresetRef& to) = 0;
    virtual bool remove(const PresetRef& ref) = 0;

    virtual bool folded(std::string_view heading) = 0;
    virtual void setFolded(std::string_view heading, bool folded) = 0;
};

}  // namespace bambi::ui
