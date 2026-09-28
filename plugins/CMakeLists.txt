# SPDX-License-Identifier: GPL-3.0-or-later
#
# Everything every bambi plugin needs to be a plugin. Configured only with -DBAMBI_BUILD_PLUGIN=ON,
# in its own tree:
#   cmake -B build-plugin -G Ninja -DBAMBI_BUILD_PLUGIN=ON
#   cmake --build build-plugin

# ---- identity shared by the suite; each plugin adds its own name and codes ---------------------
set(BAMBI_COMPANY_NAME      "bambi" CACHE STRING "Manufacturer name shown by hosts")
set(BAMBI_MANUFACTURER_CODE "Bamb"   CACHE STRING "Four characters, at least one upper-case (AU)")
option(BAMBI_COPY_PLUGIN "Install built plugins into ~/Library/Audio/Plug-Ins after building" ON)

# The format rules are checked here, at configure time, rather than discovered in a host.
# Kept as a function so plugin #2 cannot forget it.
function(bambi_check_codes manufacturer plugin_code)
  string(LENGTH "${manufacturer}" _len)
  string(REGEX MATCHALL "[A-Z]" _upper "${manufacturer}")
  list(LENGTH _upper _nupper)
  if(NOT _len EQUAL 4 OR _nupper LESS 1)
    message(FATAL_ERROR "BAMBI_MANUFACTURER_CODE '${manufacturer}' must be four characters "
                        "with at least one upper-case letter")
  endif()
  string(LENGTH "${plugin_code}" _len)
  string(REGEX MATCHALL "[A-Z]" _upper "${plugin_code}")
  list(LENGTH _upper _nupper)
  if(NOT _len EQUAL 4 OR NOT _nupper EQUAL 1)
    message(FATAL_ERROR "plugin code '${plugin_code}' must be four characters "
                        "with exactly one upper-case letter")
  endif()
endfunction()

# ---- dependencies: JUCE (submodule), clap-juce-extensions (vendored, patched) ------------------
# Added once here, so a second plugin costs no extra configure time.
add_subdirectory(${PROJECT_SOURCE_DIR}/third_party/JUCE
                 ${CMAKE_BINARY_DIR}/third_party/JUCE EXCLUDE_FROM_ALL)
add_subdirectory(${PROJECT_SOURCE_DIR}/third_party/clap-juce-extensions
                 ${CMAKE_BINARY_DIR}/third_party/clap-juce-extensions EXCLUDE_FROM_ALL)

# JUCE's modules are compiled into every library that uses them -- the interface, the host, the editor and each
# plugin -- so they are configured once, here, for all of them. No web view, no curl, no VST2.
add_compile_definitions(JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)

add_subdirectory(${PROJECT_SOURCE_DIR}/ui ${CMAKE_BINARY_DIR}/ui)
add_subdirectory(${PROJECT_SOURCE_DIR}/host ${CMAKE_BINARY_DIR}/host)
add_subdirectory(${PROJECT_SOURCE_DIR}/editor ${CMAKE_BINARY_DIR}/editor)

# A console app over plugin code, with no host: a check suite, a picture tool, a bench. Its main thread
# gets the 8 MB stack macOS gives, where Windows would give 1 MB: a check holds several processors on
# the stack, at about 290 KB each.
function(bambi_plugin_tool target)
  cmake_parse_arguments(T "" "NAME" "SOURCES;LIBRARIES" ${ARGN})
  juce_add_console_app(${target} PRODUCT_NAME "${target}")
  target_sources(${target} PRIVATE ${T_SOURCES})
  target_compile_definitions(${target} PRIVATE "JucePlugin_Name=\"${T_NAME}\"")
  target_link_libraries(${target}
      PRIVATE ${T_LIBRARIES} juce::juce_recommended_config_flags juce::juce_recommended_lto_flags)
  if(MSVC)
    target_link_options(${target} PRIVATE /STACK:8388608)
  endif()
endfunction()

add_subdirectory(encoder)
add_subdirectory(echo)
add_subdirectory(reverb)
add_subdirectory(${PROJECT_SOURCE_DIR}/tests ${CMAKE_BINARY_DIR}/tests)
