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
#include "bambi/live/pairs.hpp"
#include "bambi/live/transport.hpp"
#include "bambi/patch/json.hpp"

/*  The host's audio: the loop device opened as the host's own, an Encoder for every track on one of its pairs and
 *  the Decoder, played by the runner. Live's tracks arrive on the pairs; the decoded mix goes out on the pair past
 *  them, which Live's bambi return plays. A track keeps its Encoder, and what it is set to, for as long as it is on
 *  a pair; one new to a pair gets one loaded, and one gone lets go of its own -- each change a new program for the
 *  runner, swapped in with a short fade.
 *
 *  The plugins' states, as the set keeps them: each Encoder's by its track, the Decoder's, each base64. A state
 *  restored for a track not yet on a pair waits for it.
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

    /// The tracks on pairs now. Loads an Encoder for each track new to a pair, lets go of each whose track has
    /// gone, and swaps the new program in. True when the Encoders changed. Message thread; loading takes a moment
    /// per Encoder.
    bool setTracks(const std::vector<live::TrackPair>& pairs, juce::String& status);

    /// Frees the programs and Encoders the audio thread has let go of. Message thread.
    void tidy() { runner_.tidy(); }

    /// Every plugin's state, as the set keeps it.
    Json states() const;
    /// States the set gave back, into the plugins they belong to; one for a track not on a pair waits for it.
    void restore(const Json& states);

    /// The host's own part of the spatial mix's lateness: the loop device both ways, the device's buffer and the
    /// plan. Live's own part comes on top.
    double latencyMs() const { return latencyMs_; }

    /// The Encoder of the track on `pair`; none when no track is on it.
    juce::AudioProcessor* encoder(int pair) const;
    juce::AudioProcessor* decoder() const { return decoder_.get(); }

private:
    struct Voice {  // a track on a pair, and its Encoder
        live::TrackId track{0};
        int pair{-1};
        std::shared_ptr<juce::AudioPluginInstance> encoder;
    };

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
    std::vector<Voice> voices_;
    std::shared_ptr<juce::AudioPluginInstance> decoder_;
    Json waiting_ = Json::object();  ///< restored Encoder states, by track, for tracks not yet on a pair
    int block_{64};
    Runner runner_;
    live::TransportProjector projector_;
    std::optional<live::Arrivals> arrivals_;
    SpscQueue<Timed, 256> queue_;  ///< full only while no block runs; a message then is dropped
    int loopChannels_{16};
    int returnChannel_{14};
    double latencyMs_{0.0};
    int deviceLatency_{0};               ///< the loop device both ways, and its buffer
    std::atomic<bool> playable_{false};  ///< the device runs at the rate the plugins were prepared for
    std::int64_t faded_{0};              ///< samples of the fade-in played
};

}  // namespace bambi::livehost
