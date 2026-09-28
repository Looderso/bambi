// SPDX-License-Identifier: GPL-3.0-or-later
#include "PluginProcessor.h"

/*  The wrapper's entry point, on its own so a harness can link two plugins' code at once, which
 *  one check needs to put two products on one bus. Only the plugin targets compile this file; a
 *  console app supplies its own main.
 */
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new BambiEchoProcessor(); }
