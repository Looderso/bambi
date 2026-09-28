# SPDX-License-Identifier: GPL-3.0-or-later
#
# bambi Echo. The suite's shared plugin setup -- JUCE, clap-juce-extensions, the manufacturer
# identity and the format code rules -- is in ../CMakeLists.txt, and everything a plugin does with
# a host that is not Echo's own is in bambi-host.

set(BAMBI_ECHO_NAME "bambi Echo"     CACHE STRING "Plugin name shown by hosts")
set(BAMBI_ECHO_CODE "Bech"            CACHE STRING "Four characters, exactly one upper-case (AU)")
set(BAMBI_ECHO_BUNDLE_ID "org.bambi.echo" CACHE STRING "Reverse-DNS bundle id, also the CLAP id")

bambi_check_codes("${BAMBI_MANUFACTURER_CODE}" "${BAMBI_ECHO_CODE}")
message(STATUS "plugin identity: ${BAMBI_COMPANY_NAME} (${BAMBI_MANUFACTURER_CODE}) / "
               "${BAMBI_ECHO_NAME} (${BAMBI_ECHO_CODE}) / ${BAMBI_ECHO_BUNDLE_ID}")

juce_add_plugin(BambiEcho
    COMPANY_NAME "${BAMBI_COMPANY_NAME}"
    PLUGIN_NAME "${BAMBI_ECHO_NAME}"
    PRODUCT_NAME "${BAMBI_ECHO_NAME}"
    PLUGIN_MANUFACTURER_CODE ${BAMBI_MANUFACTURER_CODE}
    PLUGIN_CODE ${BAMBI_ECHO_CODE}
    BUNDLE_ID "${BAMBI_ECHO_BUNDLE_ID}"
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
add_library(bambi-echo-code INTERFACE)
target_sources(bambi-echo-code INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/PluginProcessor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/PluginEditor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/UI/Controls.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/UI/Panel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/UI/Strip.cpp)
target_include_directories(bambi-echo-code INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
target_compile_definitions(bambi-echo-code INTERFACE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(bambi-echo-code INTERFACE
    bambi-echo
    bambi-host
    bambi-ui
    bambi-editor
    clap_juce_extensions
    juce::juce_audio_utils)

#  The wrapper's entry point is the PLUGIN's, not the shared code's: a harness may link two
#  plugins' sources at once, and two createPluginFilter definitions do not link.
target_sources(BambiEcho PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source/PluginEntry.cpp)

target_link_libraries(BambiEcho
    PRIVATE
        bambi-echo-code
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

clap_juce_extensions_plugin(TARGET BambiEcho
    CLAP_ID "${BAMBI_ECHO_BUNDLE_ID}"
    CLAP_FEATURES audio-effect ambisonic surround)

# bambi-echo-check: the same host-contract properties the encoder's check proves, on a FIELD
# EFFECT -- layout acceptance, block-size independence, state, the handoff -- headless, before
# anyone opens a DAW. Exit code 0 means every check passed.
bambi_plugin_tool(bambi-echo-check NAME "${BAMBI_ECHO_NAME}" SOURCES Check/EchoCheck.cpp LIBRARIES bambi-echo-code)

# bambi-echo-shot: a PNG of the editor, so a UI change is looked at rather than assumed.
bambi_plugin_tool(bambi-echo-shot NAME "${BAMBI_ECHO_NAME}" SOURCES Check/Shot.cpp LIBRARIES bambi-echo-code)
