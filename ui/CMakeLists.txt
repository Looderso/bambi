# SPDX-License-Identifier: GPL-3.0-or-later
#
# bambi-ui -- the look and the shared components of every plugin in the suite: Theme.h names every
# colour, size, stroke, radius and spacing (tools/check-style.sh fails on a literal anywhere else), and
# the components draw from it. It depends on JUCE's graphics and GUI modules and on bambi-core, never on
# a processor or a plugin.
add_library(bambi-ui STATIC
  src/HitArea.cpp
  src/SceneStrip.cpp
  src/EnergyLayer.cpp
  src/EditorFrame.cpp
  src/EnvelopeGraph.cpp
  src/ParameterPage.cpp
  src/ProbeView.cpp
  src/RegionClipboard.cpp
  src/RegionEditor.cpp
  src/SettingsPage.cpp
  src/RegionOverlay.cpp
  src/SourceSettings.cpp
  src/Tiles.cpp
  src/MatrixView.cpp
  src/Header.cpp
  src/HostDrag.cpp
  src/LevelColumn.cpp
  src/StatusFooter.cpp
  src/Strokes.cpp
  src/TextField.cpp
  src/PresetBrowser.cpp
  src/ConfirmBox.cpp
  src/Tooltips.cpp
  src/Widgets.cpp
)
target_include_directories(bambi-ui PUBLIC include)
target_link_libraries(bambi-ui PUBLIC juce::juce_graphics juce::juce_gui_basics bambi-core)
bambi_warnings(bambi-ui)
