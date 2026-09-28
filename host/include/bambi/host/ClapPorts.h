// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <clap-juce-extensions/clap-juce-extensions.h>
#include <cstdio>
#include <juce_audio_processors/juce_audio_processors.h>

#include "bambi/host/Layout.h"
#include "bambi/math/sh.hpp"

/*  What a CLAP host is told about a field effect's ports.
 *
 *  A CLAP host learns a plugin's ambisonic ports and its port configurations from extensions, not
 *  from the bus layout -- so a plugin that answers `isBusesLayoutSupported` correctly and says
 *  nothing here still cannot be put on an ambisonic track by a host that asks.
 *
 *  A field effect differs from the encoder in one way: both its main ports are ambisonic, at the
 *  same order.
 */
namespace bambi::host {

/// The channel set for an order, as a layout can express it.
inline juce::AudioChannelSet ambisonicSet(int order) {
    const auto set = juce::AudioChannelSet::ambisonic(order);
    return set.size() == bambi::numChannels(order) ? set
                                                   : juce::AudioChannelSet::discreteChannels(bambi::numChannels(order));
}

/*  Mixed into a field effect's processor. `Base` is the processor; it needs `getChannelCountOfBus`,
 *  `getBusesLayout` and `setBusesLayout`, which every juce::AudioProcessor has. */
template <class Base>
class FieldEffectClapPorts : public clap_juce_extensions::clap_juce_audio_processor_capabilities {
public:
    /// Both main ports, unlike the encoder's, whose input is a stereo source. A sidechain is not ambisonic and is not ours to claim.
    int clapAmbisonicOrderForBus(bool isInput, int busIndex) override {
        if (busIndex != 0) return -1;
        return orderForChannels(self().getChannelCountOfBus(!isInput ? false : true, 0));
    }

    uint32_t clapAudioPortsConfigCount() override { return static_cast<uint32_t>(kMaxHostOrder); }

    bool clapAudioPortsConfigGet(uint32_t index, clap_audio_ports_config* config) override {
        if (config == nullptr || index >= static_cast<uint32_t>(kMaxHostOrder)) return false;
        const int order = static_cast<int>(index) + 1;
        const auto channels = static_cast<uint32_t>(bambi::numChannels(order));
        /*  The id is the position. CLAP says select() takes the configuration's id; REAPER instead
         *  passes the position, which lands one order short unless id and position agree. */
        config->id = static_cast<clap_id>(index);
        std::snprintf(config->name, sizeof config->name, "Ambisonic order %d (%u channels)", order, channels);
        config->input_port_count = 2;
        config->output_port_count = 1;
        config->has_main_input = true;
        config->main_input_channel_count = channels;
        config->main_input_port_type = CLAP_PORT_AMBISONIC;
        config->has_main_output = true;
        config->main_output_channel_count = channels;
        config->main_output_port_type = CLAP_PORT_AMBISONIC;
        return true;
    }

    bool clapAudioPortsConfigSelect(clap_id configId) override {
        const int order = static_cast<int>(configId) + 1;  // id == position, see clapAudioPortsConfigGet
        if (order < 1 || order > kMaxHostOrder) return false;
        auto layout = self().getBusesLayout();
        //  Both ends together: an effect that moved one would be asking for a layout it refuses.
        layout.getChannelSet(true, 0) = ambisonicSet(order);
        layout.getChannelSet(false, 0) = ambisonicSet(order);
        return self().setBusesLayout(layout);
    }

private:
    Base& self() { return static_cast<Base&>(*this); }
    const Base& self() const { return static_cast<const Base&>(*this); }
};

}  // namespace bambi::host
