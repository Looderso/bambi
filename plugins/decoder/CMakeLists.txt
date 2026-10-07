# SPDX-License-Identifier: GPL-3.0-or-later
#
# bambi Decoder. The suite's shared plugin setup -- JUCE, clap-juce-extensions, the manufacturer
# identity and the format code rules -- is in ../CMakeLists.txt, and everything a plugin does with
# a host that is not the decoder's own is in bambi-host.

set(BAMBI_DECODER_NAME "bambi Decoder"     CACHE STRING "Plugin name shown by hosts")
set(BAMBI_DECODER_CODE "Bdec"              CACHE STRING "Four characters, exactly one upper-case (AU)")
set(BAMBI_DECODER_BUNDLE_ID "org.bambi.decoder" CACHE STRING "Reverse-DNS bundle id, also the CLAP id")

bambi_check_codes("${BAMBI_MANUFACTURER_CODE}" "${BAMBI_DECODER_CODE}")
message(STATUS "plugin identity: ${BAMBI_COMPANY_NAME} (${BAMBI_MANUFACTURER_CODE}) / "
               "${BAMBI_DECODER_NAME} (${BAMBI_DECODER_CODE}) / ${BAMBI_DECODER_BUNDLE_ID}")

juce_add_plugin(BambiDecoder
    COMPANY_NAME "${BAMBI_COMPANY_NAME}"
    PLUGIN_NAME "${BAMBI_DECODER_NAME}"
    PRODUCT_NAME "${BAMBI_DECODER_NAME}"
    PLUGIN_MANUFACTURER_CODE ${BAMBI_MANUFACTURER_CODE}
    PLUGIN_CODE ${BAMBI_DECODER_CODE}
    BUNDLE_ID "${BAMBI_DECODER_BUNDLE_ID}"
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

# The head model ships inside the plugin: nobody's head and the template, learned from HUTUBS and 3D3A.
juce_add_binary_data(bambi-decoder-data SOURCES ${PROJECT_SOURCE_DIR}/core/data/decode/head.json)
set_target_properties(bambi-decoder-data PROPERTIES POSITION_INDEPENDENT_CODE ON)

bambi_plugin_code(bambi-decoder-code NAME "${BAMBI_DECODER_NAME}" LIBRARIES bambi-decode bambi-decoder-data juce::juce_osc
    SOURCES
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/UI/Controls.cpp
        Source/UI/SceneView.cpp
        Source/UI/HeadStrip.cpp
        Source/UI/Panel.cpp)

#  The wrapper's entry point is the PLUGIN's, not the shared code's: a harness may link two
#  plugins' sources at once, and two createPluginFilter definitions do not link.
target_sources(BambiDecoder PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source/PluginEntry.cpp)

target_link_libraries(BambiDecoder
    PRIVATE
        bambi-decoder-code
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

clap_juce_extensions_plugin(TARGET BambiDecoder
    CLAP_ID "${BAMBI_DECODER_BUNDLE_ID}"
    CLAP_FEATURES audio-effect ambisonic surround)

# bambi-decoder-check: the decoder's host contract -- an ambisonic input at the order that fits, an output as wide
# as the host gives, every output a set of channels on it -- and its renders. Exit code 0 means every check passed.
bambi_plugin_tool(bambi-decoder-check NAME "${BAMBI_DECODER_NAME}" SOURCES Check/DecoderCheck.cpp
                  LIBRARIES bambi-decoder-code)

# bambi-decoder-shot: a PNG of the editor, so a UI change is looked at rather than assumed.
bambi_plugin_tool(bambi-decoder-shot NAME "${BAMBI_DECODER_NAME}" SOURCES Check/Shot.cpp LIBRARIES bambi-decoder-code)
