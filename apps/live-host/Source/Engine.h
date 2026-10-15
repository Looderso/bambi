// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include <optional>
#include <vector>

#include "Runner.h"
#include "bambi/link/spsc.hpp"
#include "bambi/live/arrival.hpp"
#include "bambi/live/transport.hpp"

/*  The host's audio: the loop device opened as the host's own, an Encoder for each of its pairs and the Decoder,
 *  played by the runner. Live's tracks arrive on the pairs; the decoded mix goes out on the pair past them, which
 *  Live's bambi return plays. Every pair has its Encoder whether a track is on it or not, so a track added or moved
 *  in Live changes no plugin here.
 *
 *  Live's transport arrives on the message thread, each message stamped with when its line was read; the audio
 *  thread takes it from a queue at the start of its next block and places it on the host's samples. It follows a
 *  clock outside the host, so what plays depends on when the messages came: this path is real-time only.
 *
 *  start() and transport() on the message thread; the callbacks are the audio device's.
 */
namespace bambi::livehost {

class Engine final : private juce::AudioIODeviceCallback {
public:
    struct Settings {
        int order{3};
        int pairs{7};
        int bufferSize{64};
        double rate{48000.0};
        std::int64_t transportOffsetSamples{0};  ///< by how much Live's audio for a position follows its report
    };

    Engine() = default;
    ~Engine() override;

    /// Opens the loop device -- bambi's own, else BlackHole 16ch -- loads the plugins and starts. False, with
    /// `status` saying why, when any of that fails; otherwise `status` says what runs.
    bool start(const Settings& settings, juce::String& status);

    /// Live's transport, as the device sent it, stamped in steady-clock nanoseconds when its line was read.
    void transport(const live::TransportMessage& message, std::int64_t stampNs) noexcept;

    /// The host's own part of the spatial mix's lateness: the loop device both ways, the device's buffer and the
    /// plan. Live's own part comes on top.
    double latencyMs() const { return latencyMs_; }

    juce::AudioProcessor* encoder(int pair) const;
    juce::AudioProcessor* decoder() const;

private:
    struct Timed {
        live::TransportMessage message;
        std::int64_t stampNs{0};
    };

    void audioDeviceIOCallbackWithContext(const float* const* inputs, int inputCount, float* const* outputs,
                                          int outputCount, int frames,
                                          const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

    Settings s_;
    juce::AudioDeviceManager devices_;
    std::vector<std::unique_ptr<juce::AudioPluginInstance>> plugins_;  ///< the pairs' Encoders, then the Decoder
    Runner runner_;
    live::TransportProjector projector_;
    std::optional<live::Arrivals> arrivals_;
    SpscQueue<Timed, 256> queue_;  ///< full only while no block runs; a message then is dropped
    int loopChannels_{16};
    int returnChannel_{14};
    double latencyMs_{0.0};
    std::atomic<bool> playable_{false};  ///< the device runs at the rate the plugins were prepared for
    std::int64_t faded_{0};              ///< samples of the fade-in played
};

}  // namespace bambi::livehost
