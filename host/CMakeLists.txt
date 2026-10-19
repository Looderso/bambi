# SPDX-License-Identifier: GPL-3.0-or-later
#
# bambi-host — the shared processor base.
#
# What a plugin does with a HOST is the same in all three: it declares parameters from its manifest,
# answers questions about buses, carries a patch with undo, hands state to the audio thread without a
# lock, and joins the link bus. Only the engine and the control step differ.

add_library(bambi-host STATIC
  src/Layout.cpp
  src/LinkNode.cpp
  src/ControlTarget.cpp
  src/Parameters.cpp
  src/PluginProcessor.cpp
  src/FieldEffectProcessor.cpp
)
target_include_directories(bambi-host PUBLIC include)
# clap_juce_extensions: FieldEffectProcessor is a CLAP-capable processor, and ClapPorts.h names its headers.
target_link_libraries(bambi-host PUBLIC bambi-core bambi-juce-headers bambi-remote clap_juce_extensions)
bambi_warnings(bambi-host)
# Link-time optimisation, as every plugin target has: FieldEffectProcessor is the audio thread's
# outer loop, and with LTO it costs what it would compiled inside each plugin (within 0.4 %).
target_link_libraries(bambi-host PRIVATE juce::juce_recommended_lto_flags)

# bambi-remote — the interface through which a host that knows the suite sets one of its plugins: the plugins
# answer it, the live host asks it. Only the VST3 SDK's base headers, which JUCE carries.
add_library(bambi-remote INTERFACE)
target_include_directories(bambi-remote INTERFACE remote/include)
target_link_libraries(bambi-remote INTERFACE juce_vst3_headers)
