# SPDX-License-Identifier: GPL-3.0-or-later
#
# bambi live host: the app that plays a Live set's tracks through the suite's own installed plugins. It and its
# check and picture tool link the JUCE built to host VST3 (bambi-juce-hosting); it sits above every library of the
# suite: nothing includes it.

set(BAMBI_MARK "${PROJECT_SOURCE_DIR}/ui/assets/marks/bambi.svg")
juce_add_gui_app(bambi-live-host PRODUCT_NAME "bambi live host" COMPANY_NAME "${BAMBI_COMPANY_NAME}"
                 BUNDLE_ID org.bambi.livehost ICON_BIG "${BAMBI_MARK}"
                 MICROPHONE_PERMISSION_ENABLED TRUE
                 MICROPHONE_PERMISSION_TEXT "bambi live host plays the tracks Live sends it through the loop device.")
# JUCE renders the icon while configuring; an edit to the mark configures again.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${BAMBI_MARK}")
target_sources(bambi-live-host PRIVATE Source/Main.cpp Source/Device.cpp Source/Engine.cpp Source/Rail.cpp Source/Routing.cpp Source/Plugins.cpp Source/Runner.cpp Source/Settings.cpp Source/IoPage.cpp Source/Timeline.cpp Source/LiveDevice.cpp)
target_compile_definitions(bambi-live-host PRIVATE JUCE_APPLICATION_NAME_STRING="bambi live host"
                                                   JUCE_APPLICATION_VERSION_STRING="0.1.0")
target_link_libraries(bambi-live-host
    PRIVATE bambi-live bambi-encode bambi-echo bambi-reverb bambi-loop bambi-net bambi-devices bambi-remote bambi-ui bambi-juce-hosting
            juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# The Max for Live device travels in the bundle, which the host copies into Live's User Library at start: the
# device and the scripts it loads, not its generator or its checks. As sources, so an edit to one is copied again.
if(APPLE)
  file(GLOB BAMBI_LIVE_DEVICE CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/device/bambi-live*.amxd
                                                ${CMAKE_CURRENT_SOURCE_DIR}/device/bambi-live*.js)
  target_sources(bambi-live-host PRIVATE ${BAMBI_LIVE_DEVICE})
  set_source_files_properties(${BAMBI_LIVE_DEVICE} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/device)
endif()

# macOS asks for the microphone again for every new signature, and holds the host's first input until it is
# answered; an ad-hoc signature is new with every build. Signed with a fixed identity, the answer is kept.
set(BAMBI_HOST_SIGN_IDENTITY "" CACHE STRING "A code signing identity for the live host; empty: ad hoc")
if(APPLE AND BAMBI_HOST_SIGN_IDENTITY)
  add_custom_command(TARGET bambi-live-host POST_BUILD
                     COMMAND codesign --force --sign "${BAMBI_HOST_SIGN_IDENTITY}" "$<TARGET_BUNDLE_DIR:bambi-live-host>"
                     VERBATIM)
endif()

# The runner against a direct render, through the real installed plugins, at every block size. Headless.
juce_add_console_app(bambi-live-host-check PRODUCT_NAME "bambi-live-host-check")
target_sources(bambi-live-host-check PRIVATE Check/RunnerCheck.cpp Source/Engine.cpp Source/Plugins.cpp Source/Runner.cpp Source/Settings.cpp Source/IoPage.cpp Source/Timeline.cpp Source/Routing.cpp Source/Rail.cpp)
target_link_libraries(bambi-live-host-check
    PRIVATE bambi-live bambi-decode bambi-encode bambi-echo bambi-reverb bambi-loop bambi-devices bambi-remote bambi-ui bambi-juce-hosting
            juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# Draws the window to a PNG over a set built in the tool: no Live, no audio device, no installed plugin.
juce_add_console_app(bambi-live-host-shot PRODUCT_NAME "bambi-live-host-shot")
target_sources(bambi-live-host-shot PRIVATE Check/Shot.cpp Source/Rail.cpp Source/Routing.cpp Source/Timeline.cpp Source/IoPage.cpp Source/Plugins.cpp)
target_link_libraries(bambi-live-host-shot
    PRIVATE bambi-live bambi-decode bambi-encode bambi-echo bambi-reverb bambi-remote bambi-ui bambi-juce-hosting
            juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
target_include_directories(bambi-live-host-shot PRIVATE ${PROJECT_SOURCE_DIR}/host/include)  # ShotScale.h, inline
