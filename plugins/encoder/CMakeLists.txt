# SPDX-License-Identifier: GPL-3.0-or-later
#
# bambi Encoder. The suite's shared plugin setup -- JUCE, clap-juce-extensions, the manufacturer
# identity and the format code rules -- is in ../CMakeLists.txt.

# ---- identity: configurable while developing, frozen at release -------------
set(BAMBI_PLUGIN_NAME "bambi Encoder"     CACHE STRING "Plugin name shown by hosts")
set(BAMBI_PLUGIN_CODE "Benc"               CACHE STRING "Four characters, exactly one upper-case (AU)")
set(BAMBI_BUNDLE_ID   "org.bambi.encoder" CACHE STRING "Reverse-DNS bundle id, also the CLAP id")

bambi_check_codes("${BAMBI_MANUFACTURER_CODE}" "${BAMBI_PLUGIN_CODE}")
message(STATUS "plugin identity: ${BAMBI_COMPANY_NAME} (${BAMBI_MANUFACTURER_CODE}) / "
               "${BAMBI_PLUGIN_NAME} (${BAMBI_PLUGIN_CODE}) / ${BAMBI_BUNDLE_ID}")

juce_add_plugin(BambiEncoder
    COMPANY_NAME "${BAMBI_COMPANY_NAME}"
    PLUGIN_NAME "${BAMBI_PLUGIN_NAME}"
    PRODUCT_NAME "${BAMBI_PLUGIN_NAME}"
    PLUGIN_MANUFACTURER_CODE ${BAMBI_MANUFACTURER_CODE}
    PLUGIN_CODE ${BAMBI_PLUGIN_CODE}
    BUNDLE_ID "${BAMBI_BUNDLE_ID}"
    FORMATS VST3                     # and CLAP, below.
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES Fx Spatial
    COPY_PLUGIN_AFTER_BUILD ${BAMBI_COPY_PLUGIN}
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE)

bambi_plugin_code(bambi-encoder-code NAME "${BAMBI_PLUGIN_NAME}" LIBRARIES bambi-encode
    SOURCES
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/UI/SceneModel.cpp
        Source/UI/SphereView.cpp
        Source/UI/Panels.cpp
        Source/UI/Controls.cpp
        Source/UI/ParameterTabs.cpp
        Source/UI/SourceSettings.cpp
        Source/UI/FrameTrace.cpp)

#  The wrapper's entry point is the PLUGIN's, not the shared code's: the conformance suite links
#  every plugin's sources at once, and three createPluginFilter definitions do not link.
target_sources(BambiEncoder PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source/PluginEntry.cpp)

target_link_libraries(BambiEncoder
    PRIVATE
        bambi-encoder-code
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

clap_juce_extensions_plugin(TARGET BambiEncoder
    CLAP_ID "${BAMBI_BUNDLE_ID}"
    CLAP_FEATURES audio-effect ambisonic surround)

# ---- a console app over this plugin's own code, with no host ---------------------------------
function(bambi_encoder_tool target source)
  bambi_plugin_tool(${target} NAME "${BAMBI_PLUGIN_NAME}" SOURCES ${source} LIBRARIES bambi-encoder-code)
endfunction()

# bambi-plugin-bench: REAPER's CPU meter cannot separate the audio thread from a window redrawing
# on the message thread. This times each, on the shipped sources.
bambi_encoder_tool(bambi-plugin-bench Bench/PluginBench.cpp)

# bambi-plugin-check: host-contract properties that need no host -- layout acceptance, input-width
# parity, block-size independence, on the shipped sources, before anyone opens a DAW. Exit code 0
# means every check passed.
bambi_encoder_tool(bambi-plugin-check Check/PluginCheck.cpp)

# bambi-plugin-snapshot: five real processors on one link bus, run for a moment, and the editor
# drawn over them -- so a UI change is compared with the draft in seconds, without a host.
bambi_encoder_tool(bambi-plugin-snapshot Snapshot/Snapshot.cpp)

