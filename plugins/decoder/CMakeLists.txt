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

# Two heads ship inside the plugin: nobody's head and the template, learned from HUTUBS and 3D3A, and the KU100 as
# measured (SADIE II, Apache 2.0), fitted into the same tables by tools/head.
juce_add_binary_data(bambi-decoder-data SOURCES
    ${PROJECT_SOURCE_DIR}/core/data/decode/head.json
    ${PROJECT_SOURCE_DIR}/core/data/decode/ku100.json)
set_target_properties(bambi-decoder-data PROPERTIES POSITION_INDEPENDENT_CODE ON)

bambi_plugin_code(bambi-decoder-code NAME "${BAMBI_DECODER_NAME}" LIBRARIES bambi-decode bambi-decoder-data qrcodegen juce::juce_osc
    SOURCES
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/HelperLink.mm
        Source/UI/Controls.cpp
        Source/UI/SceneView.cpp
        Source/UI/HeadStrip.cpp
        Source/UI/Panel.cpp)
#  Automatic reference counting for the launcher: it keeps the helper's NSRunningApplication across calls. On the
#  other platforms the file is its C++ stub, compiled as C++.
if(APPLE)
  set_source_files_properties(Source/HelperLink.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
else()
  set_source_files_properties(Source/HelperLink.mm PROPERTIES LANGUAGE CXX)
endif()

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
target_compile_definitions(bambi-decoder-check PRIVATE BAMBI_CORE_DIR="${PROJECT_SOURCE_DIR}/core")  # the SOFA fixture

# bambi-decoder-shot: a PNG of the editor, so a UI change is looked at rather than assumed.
bambi_plugin_tool(bambi-decoder-shot NAME "${BAMBI_DECODER_NAME}" SOURCES Check/Shot.cpp LIBRARIES bambi-decoder-code)

# The AirPods helper: a separate app, because macOS asks the requesting process for a motion usage text and no host
# carries one. Built here, nested into both bundles' Contents/Helpers after they are built and copied, and the
# bundles signed again. Launched by the decoder through Launch Services.
if(APPLE)
  #  The phone page travels inside the helper as bytes, generated from Helper/phone.html at configure time.
  file(READ ${CMAKE_CURRENT_SOURCE_DIR}/Helper/phone.html PHONE_HTML HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," PHONE_HTML_BYTES "${PHONE_HTML}")
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/helper/phone_page.h
       "/* generated from Helper/phone.html */\nstatic const unsigned char kPhonePage[] = {${PHONE_HTML_BYTES}};\nstatic const unsigned long kPhonePageSize = sizeof kPhonePage;\n")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Helper/phone.html)
  add_executable(bambi-decoder-helper MACOSX_BUNDLE Helper/main.m Helper/server.m)
  set_source_files_properties(Helper/main.m Helper/server.m PROPERTIES COMPILE_FLAGS "-fobjc-arc")
  target_include_directories(bambi-decoder-helper PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/helper)
  set_target_properties(bambi-decoder-helper PROPERTIES
      OUTPUT_NAME "bambi Decoder Helper"
      MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Helper/Info.plist)
  target_link_libraries(bambi-decoder-helper PRIVATE "-framework Cocoa" "-framework CoreMotion" "-framework Network"
                                                     "-framework Security")
  target_compile_options(bambi-decoder-helper PRIVATE -mmacosx-version-min=14.0)
  target_link_options(bambi-decoder-helper PRIVATE -mmacosx-version-min=14.0)
  #  Nested after whichever side was built: the plugin, or the helper alone, whose change would otherwise never reach
  #  the installed bundles. The script skips a bundle that is not there yet.
  foreach(format VST3 CLAP)
    add_dependencies(BambiDecoder_${format} bambi-decoder-helper)
    string(TOLOWER "${format}" ext)
    set(installed "$ENV{HOME}/Library/Audio/Plug-Ins/${format}/${BAMBI_DECODER_NAME}.${ext}")
    foreach(built BambiDecoder_${format} bambi-decoder-helper)
      add_custom_command(TARGET ${built} POST_BUILD
          COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/Helper/nest.sh "$<TARGET_BUNDLE_DIR:bambi-decoder-helper>"
                  "$<TARGET_BUNDLE_DIR:BambiDecoder_${format}>" "${installed}"
          COMMENT "nesting the helper into the ${format} bundle")
    endforeach()
  endforeach()
endif()
