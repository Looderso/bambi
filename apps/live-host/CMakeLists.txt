# SPDX-License-Identifier: GPL-3.0-or-later
#
# bambi live host: the app that plays a Live set's tracks through the suite's own installed plugins. Its own JUCE,
# built to host VST3 -- which the shared one is not -- and above every library of the suite: nothing includes it.

juce_add_gui_app(bambi-live-host PRODUCT_NAME "bambi live host" COMPANY_NAME "${BAMBI_COMPANY_NAME}"
                 BUNDLE_ID org.bambi.livehost MICROPHONE_PERMISSION_ENABLED TRUE
                 MICROPHONE_PERMISSION_TEXT "bambi live host plays the tracks Live sends it through the loop device.")
target_sources(bambi-live-host PRIVATE Source/Main.cpp Source/Device.cpp Source/Engine.cpp Source/Rail.cpp Source/Routing.cpp Source/Plugins.cpp Source/Runner.cpp Source/Settings.cpp)
target_compile_definitions(bambi-live-host PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0
                                                   JUCE_APPLICATION_NAME_STRING="bambi live host"
                                                   JUCE_APPLICATION_VERSION_STRING="0.1.0")
target_link_libraries(bambi-live-host
    PRIVATE bambi-live bambi-loop bambi-net bambi-devices bambi-remote bambi-ui juce::juce_audio_processors juce::juce_audio_devices juce::juce_gui_basics
            juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

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
target_sources(bambi-live-host-check PRIVATE Check/RunnerCheck.cpp Source/Plugins.cpp Source/Runner.cpp Source/Settings.cpp)
target_compile_definitions(bambi-live-host-check PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
target_link_libraries(bambi-live-host-check
    PRIVATE bambi-live bambi-decode bambi-devices bambi-remote juce::juce_audio_processors juce::juce_audio_devices
            juce::juce_gui_basics
            juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
