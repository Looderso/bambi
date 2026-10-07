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
        Source/HelperLink.mm
        Source/UI/Controls.cpp
        Source/UI/SceneView.cpp
        Source/UI/HeadStrip.cpp
        Source/UI/Panel.cpp)
#  Automatic reference counting for the launcher: it keeps the helper's NSRunningApplication across calls.
set_source_files_properties(Source/HelperLink.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")

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

# The AirPods helper: a separate app, because macOS asks the requesting process for a motion usage text and no host
# carries one. Built here, nested into both bundles' Contents/Helpers after they are built and copied, and the
# bundles signed again. Launched by the decoder through Launch Services.
if(APPLE)
  add_executable(bambi-decoder-helper MACOSX_BUNDLE Helper/main.m)
  set_source_files_properties(Helper/main.m PROPERTIES COMPILE_FLAGS "-fobjc-arc")
  set_target_properties(bambi-decoder-helper PROPERTIES
      OUTPUT_NAME "bambi Decoder Helper"
      MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Helper/Info.plist)
  target_link_libraries(bambi-decoder-helper PRIVATE "-framework Cocoa" "-framework CoreMotion")
  target_compile_options(bambi-decoder-helper PRIVATE -mmacosx-version-min=14.0)
  target_link_options(bambi-decoder-helper PRIVATE -mmacosx-version-min=14.0)
  foreach(format VST3 CLAP)
    add_dependencies(BambiDecoder_${format} bambi-decoder-helper)
    string(TOLOWER "${format}" ext)
    set(installed "$ENV{HOME}/Library/Audio/Plug-Ins/${format}/${BAMBI_DECODER_NAME}.${ext}")
    add_custom_command(TARGET BambiDecoder_${format} POST_BUILD
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/Helper/nest.sh "$<TARGET_BUNDLE_DIR:bambi-decoder-helper>"
                "$<TARGET_BUNDLE_DIR:BambiDecoder_${format}>" "${installed}"
        COMMENT "nesting the helper into the ${format} bundle")
  endforeach()
endif()
