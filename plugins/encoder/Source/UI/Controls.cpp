// SPDX-License-Identifier: GPL-3.0-or-later
#include "UI/Controls.h"

namespace bambi::ui {

ControlState::ControlState(BambiEncoderProcessor& p)
    : bambi::editor::PatchControls(p, bambi::encodeParams(), bambi::encodeMod(), 3), processor(p) {}

}  // namespace bambi::ui
