# SPDX-License-Identifier: GPL-3.0-or-later
#
# The conformance suite: every plugin, in both formats, asked the same questions about its buses.
#
# It links EVERY plugin's code at once, which is why each plugin's `createPluginFilter` lives in its
# own PluginEntry.cpp. One binary rather than one per plugin, because the point is
# that the three answer the same questions the same way.

bambi_plugin_tool(bambi-conformance NAME "bambi"
    SOURCES conformance/main.cpp conformance/Conformance.cpp conformance/InstalledVst3.cpp conformance/InstalledClap.cpp
    LIBRARIES bambi-encoder-code bambi-echo-code bambi-reverb-code)
target_compile_definitions(bambi-conformance PRIVATE JUCE_PLUGINHOST_VST3=1)
target_include_directories(bambi-conformance PRIVATE
    ${PROJECT_SOURCE_DIR}/third_party/clap-juce-extensions/clap-libs/clap/include)
