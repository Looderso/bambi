# SPDX-License-Identifier: GPL-3.0-or-later
#
# bambi-editor — where `ui` meets `host`.
#
# `ui` does not depend on `host`: what a window sees is `ui::PatchModel`, and what an edit
# reaches is `host::ControlTarget`. A plugin's control state joins the two, and the part of that join
# that is the same in every plugin is here rather than in each of them.
add_library(bambi-editor STATIC
  src/PatchControls.cpp
  src/EffectControls.cpp
  src/EffectFrame.cpp
  src/PluginEditor.cpp
  src/EffectEditor.cpp
)
target_include_directories(bambi-editor PUBLIC include)
target_link_libraries(bambi-editor PUBLIC bambi-ui bambi-host)
bambi_warnings(bambi-editor)

