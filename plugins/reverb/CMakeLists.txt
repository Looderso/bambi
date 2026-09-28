# SPDX-License-Identifier: GPL-3.0-or-later
#
# bambi Reverb. The suite's shared plugin setup -- JUCE, clap-juce-extensions, the manufacturer
# identity and the format code rules -- is in ../CMakeLists.txt, and everything a plugin does with
# a host that is not Reverb's own is in bambi-host.

set(BAMBI_REVERB_NAME "bambi Reverb"     CACHE STRING "Plugin name shown by hosts")
set(BAMBI_REVERB_CODE "Brev"            CACHE STRING "Four characters, exactly one upper-case (AU)")
set(BAMBI_REVERB_BUNDLE_ID "org.bambi.reverb" CACHE STRING "Reverse-DNS bundle id, also the CLAP id")

bambi_check_codes("${BAMBI_MANUFACTURER_CODE}" "${BAMBI_REVERB_CODE}")
message(STATUS "plugin identity: ${BAMBI_COMPANY_NAME} (${BAMBI_MANUFACTURER_CODE}) / "
               "${BAMBI_REVERB_NAME} (${BAMBI_REVERB_CODE}) / ${BAMBI_REVERB_BUNDLE_ID}")

juce_add_plugin(BambiReverb
    COMPANY_NAME "${BAMBI_COMPANY_NAME}"
    PLUGIN_NAME "${BAMBI_REVERB_NAME}"
    PRODUCT_NAME "${BAMBI_REVERB_NAME}"
    PLUGIN_MANUFACTURER_CODE ${BAMBI_MANUFACTURER_CODE}
    PLUGIN_CODE ${BAMBI_REVERB_CODE}
    BUNDLE_ID "${BAMBI_REVERB_BUNDLE_ID}"
    FORMATS VST3 Standalone          # and CLAP, below.
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES Fx Spatial
    COPY_PLUGIN_AFTER_BUILD ${BAMBI_COPY_PLUGIN}
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE)

# The plugin's own code as an INTERFACE library, so the plugin and its harnesses compile exactly the
# same sources with the same flags, each with its own copy of the JUCE modules.
add_library(bambi-reverb-code INTERFACE)
target_sources(bambi-reverb-code INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/PluginProcessor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/PluginEditor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/UI/Controls.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/UI/Panel.cpp)
target_include_directories(bambi-reverb-code INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
target_link_libraries(bambi-reverb-code INTERFACE
    bambi-reverb
    bambi-host
    bambi-ui
    bambi-editor
    clap_juce_extensions
    juce::juce_audio_utils)

#  The wrapper's entry point is the PLUGIN's, not the shared code's: a harness may link two
#  plugins' sources at once, and two createPluginFilter definitions do not link.
target_sources(BambiReverb PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source/PluginEntry.cpp)

target_link_libraries(BambiReverb
    PRIVATE
        bambi-reverb-code
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

clap_juce_extensions_plugin(TARGET BambiReverb
    CLAP_ID "${BAMBI_REVERB_BUNDLE_ID}"
    CLAP_FEATURES audio-effect ambisonic surround)

# bambi-reverb-check: the field-effect host contract again, on the suite's third plugin -- and with
# it the claim that two field effects locate, step and hand over state identically, because both
# now run on the same shared code. Exit code 0 means every check passed.
#  Echo's processor too: the one check that needs two PRODUCTS on one bus.
bambi_plugin_tool(bambi-reverb-check NAME "${BAMBI_REVERB_NAME}" SOURCES Check/ReverbCheck.cpp
                  LIBRARIES bambi-reverb-code bambi-echo-code)

# bambi-reverb-shot: a PNG of the editor, so a UI change is looked at rather than assumed.
bambi_plugin_tool(bambi-reverb-shot NAME "${BAMBI_REVERB_NAME}" SOURCES Check/Shot.cpp LIBRARIES bambi-reverb-code)
