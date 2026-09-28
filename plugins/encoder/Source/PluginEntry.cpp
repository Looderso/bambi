// SPDX-License-Identifier: GPL-3.0-or-later
#include "PluginProcessor.h"

/*  The wrapper's entry point, on its own so that a harness can link every plugin's code at once.
 *  Only the plugin target compiles this file; a console app supplies its own main.
 */
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new BambiEncoderProcessor(); }
