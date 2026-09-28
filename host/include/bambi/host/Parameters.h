// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <juce_audio_processors/juce_audio_processors.h>
#include <string_view>

#include "bambi/patch/parameters.hpp"

/*  The host parameters every plugin declares, built from its manifest.
 *
 *  `bambi-host` is the shared processor base: what a plugin does with a host is the same in all
 *  three, and only the engine and the control step differ. This is its first half -- the parameters.
 *  It knows no plugin's enum, only a `ParamManifest`.
 */
namespace bambi::host {

/// A core string as JUCE wants it.
inline juce::String toJuce(std::string_view s) { return juce::String::fromUTF8(s.data(), static_cast<int>(s.size())); }

/*  Every parameter in a manifest, as host parameters. A float takes the same skew the editor and a
 *  remote edit use (`ParamManifest::skewCentreOf`), or a normalised position would mean one rate to
 *  the host and another to the editor. Version 1 on every id, so a host's saved automation keeps
 *  pointing at it.
 */
juce::AudioProcessorValueTreeState::ParameterLayout parameterLayout(const ParamManifest& m);

/*  The plugin's own view of those parameters: the atomics the audio thread reads, the objects a
 *  gesture is made on, and the plain values handed to state and to the shared engines.
 *
 *  `values` is sized by the cap every plugin shares, because `PluginState` and `ModulationEngine`
 *  are the same shape in all three; the two arrays beside it are this plugin's own length, and there
 *  are exactly `manifest.size()` of them. Walking the wrong one past the other is how a plugin reads
 *  off the end -- which is why `intake()` exists rather than a loop at each site.
 */
class Parameters {
public:
    void attach(const ParamManifest& m, juce::AudioProcessorValueTreeState& apvts);

    /// Read every host parameter into `values`. Real-time safe: no allocation, no lock.
    void intake() noexcept;

    const ParamManifest& manifest() const { return *manifest_; }
    int count() const { return manifest_ == nullptr ? 0 : manifest_->size(); }

    /*  A parameter as the host has it now, read from the atomic rather than from `values`. `values`
     *  is written by `intake()` on the audio thread, so reading it from anywhere else is a race;
     *  this is what the message thread uses -- the link bus publishes these sixty times a second. */
    float live(int at) const noexcept {
        const auto* raw = at >= 0 && at < count() ? raw_[static_cast<std::size_t>(at)] : nullptr;
        return raw == nullptr ? 0.0f : raw->load(std::memory_order_relaxed);
    }

    std::array<float, kMaxParams> values{};
    juce::RangedAudioParameter* object(int at) const {
        return at >= 0 && at < count() ? objects_[static_cast<std::size_t>(at)] : nullptr;
    }

private:
    const ParamManifest* manifest_{nullptr};
    std::array<std::atomic<float>*, kMaxParams> raw_{};
    std::array<juce::RangedAudioParameter*, kMaxParams> objects_{};
};

}  // namespace bambi::host
