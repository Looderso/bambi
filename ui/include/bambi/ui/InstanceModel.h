// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <juce_core/juce_core.h>

#include "bambi/patch/state.hpp"

/*  The instances this window can show, and which one it is showing: all the shared header needs, and
 *  deliberately smaller than a scene. Its own kind only -- an instance in this list is one the window
 *  can select and edit, which across kinds is meaningless.
 */
namespace bambi::ui {

class InstanceModel {
public:
    virtual ~InstanceModel() = default;

    virtual int instanceCount() const = 0;
    virtual Uuid instanceAt(int index) const = 0;
    virtual juce::String instanceLabel(int index) const = 0;

    virtual Uuid selectedInstance() const = 0;
    virtual void selectInstance(const Uuid& id) = 0;
};

}  // namespace bambi::ui
