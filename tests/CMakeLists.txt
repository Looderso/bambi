# SPDX-License-Identifier: GPL-3.0-or-later
#
# The conformance suite: every plugin, in both formats, asked the same questions about its buses.
#
# It links EVERY plugin's code at once, which is why each plugin's `createPluginFilter` lives in its
# own PluginEntry.cpp. One binary rather than one per plugin, because the point is
# that the four answer the same questions the same way.

#  The JUCE built to host plugins: it loads the installed bundles.
bambi_plugin_tool(bambi-conformance NAME "bambi" HOSTING
    SOURCES conformance/main.cpp conformance/Conformance.cpp conformance/InstalledVst3.cpp conformance/InstalledClap.cpp
    LIBRARIES bambi-encoder-code bambi-echo-code bambi-reverb-code bambi-decoder-code)
target_include_directories(bambi-conformance PRIVATE
    ${PROJECT_SOURCE_DIR}/third_party/clap-juce-extensions/clap-libs/clap/include)
