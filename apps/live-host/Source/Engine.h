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
#include "bambi/live/field.hpp"
#include "bambi/decode/remote.hpp"
#include "bambi/live/io.hpp"
#include "bambi/live/lanes.hpp"
#include "bambi/live/midi.hpp"
#include "bambi/patch/parameters.hpp"
#include "bambi/live/pairs.hpp"
#include "devices.hpp"
#include "bambi/live/transport.hpp"
#include "bambi/patch/json.hpp"

/*  The host's audio: a private aggregate of the loop device and the devices this computer's settings choose, opened
 *  as the host's own -- the loop device alone while none is chosen -- an Encoder for every track on one of its
 *  pairs and the Decoders, played by the runner. Live's Main arrives on the loop device's first pair, where Live
 *  sends its Master, and the tracks on the pairs after it. The Decoder's bus is the chosen devices' outputs, channel for channel; the host asks it, through its remote
 *  interface, to place its outputs as the settings keep them, then plays them out with Main and every copy, each
 *  in step with the slowest device and Main late by the field. A copy back to Live goes out on the loop device's
 *  first pair, where Live's bambi track records it. A track keeps its Encoder, and what it is set to, for as long as it is on
 *  a pair; one new to a pair gets one loaded, and one gone lets go of its own -- each change a new program for the
 *  runner, swapped in with a short fade.
 *
 *  Each insert of the shape's chains is an Echo or a Reverb of its own, kept for as long as the insert is, and
 *  played in the same program.
 *
 *  The plugins' states, as the set keeps them: each Encoder's by its track, each insert's by its id, the
 *  Decoder's, each base64. A state restored for a track not yet on a pair, or an insert not yet playing, waits for
 *  it; so does the state of one that leaves the field, until it comes back -- or until Live no longer has the track,
 *  or the shape the insert.
 *
 *  Live's transport arrives on the message thread, each message stamped with when its line was read; the audio
 *  thread takes it from a queue at the start of its next block and places it on the host's samples. It follows a
 *  clock outside the host, so what plays depends on when the messages came: this path is real-time only.
 *
 *  MIDI, from every input the system has and from the host's own port, "bambi", where Live's MIDI tracks send: each
 *  message stamped as it arrives and placed on the host's samples by the audio thread, one buffer after it arrived
 *  -- so it keeps its place within a buffer -- and Live's instead as late as Live's audio for it reaches the host,
 *  as the settings keep it. Every instance hears all of it. The inputs are opened again whenever the system's change.
 *
 *  start() and transport() on the message thread; the callbacks are the audio device's, and the MIDI inputs'.
 */
namespace bambi::livehost {

class Engine final : private juce::AudioIODeviceCallback, private juce::MidiInputCallback {
public:
    struct Settings {
        int order{3};
        int pairs{7};
        int bufferSize{64};
        double rate{48000.0};
        std::int64_t transportOffsetSamples{0};  ///< by how much Live's audio for a position follows its report
        live::AudioSettings audio;               ///< where it plays and hears, on this computer
    };

    Engine() = default;
    ~Engine() override;

    /// Opens the aggregate, or the loop device -- bambi's own, else BlackHole 16ch -- loads the plugins and starts.
    /// False, with `status` saying why, when any of that fails; otherwise `status` says what runs.
    bool start(const Settings& settings, juce::String& status);

    /// Where it plays and hears, changed: the outputs asked of the Decoder again and played along new routes; with
    /// other devices or another buffer, the device opened again and the plugins laid out for it, which stops the
    /// sound a moment. False, with `status` saying why, when that fails. Message thread.
    bool setAudio(const live::AudioSettings& audio, juce::String& status);
    const live::AudioSettings& audio() const { return s_.audio; }
    /// The routes again, from where the Decoders report their outputs now; the program swapped only when they
    /// changed. True when they did. Message thread.
    bool reroute();
    /// Every Decoder as it plays now: where it reports its outputs -- a fresh Decoder's when it offers no
    /// interface -- and its latency.
    std::vector<live::PlayingDecoder> playing() const;
    /// The chosen devices' outputs, as the Decoder's bus has them, and their inputs.
    const live::Row& row() const { return row_; }
    const live::Row& inputRow() const { return inputRow_; }
    double rate() const { return s_.rate; }
    int order() const { return s_.order; }
    /// Where Live is heard now: the position the plugins played, less the samples from them to the speakers.
    live::Playhead heard() const noexcept;

    /// The share of each block the host's processing takes, measured as it plays and smoothed; 0 before audio.
    float load() const noexcept { return load_.load(std::memory_order_relaxed); }

    /// A track's session clip, as the device sent it, stamped as the transport is.
    void slot(const live::SlotPlay& report, std::int64_t stampNs) noexcept;

    /// Live's transport, as the device sent it, stamped in steady-clock nanoseconds when its line was read.
    void transport(const live::TransportMessage& message, std::int64_t stampNs) noexcept;

    /// The MIDI inputs the host hears, by name: the system's, then its own port. Message thread.
    std::vector<juce::String> midiInputs() const;
    /// Whether Live's lead is measured: each note-on from Live's port paired with the sound on Live's Main.
    void setMeasuring(bool on) noexcept { measuring_.store(on, std::memory_order_relaxed); }
    /// A measurement taken since the last asked, in milliseconds. Message thread.
    std::optional<double> measured() noexcept;

    /// The field as the shape lays it over Live's set, and the pairs its sources are on. Loads an Encoder for each
    /// source new to the field, lets go of each whose track has left it, and swaps the new program in. True when
    /// the program changed. Message thread; loading takes a moment per Encoder.
    /// The host's inputs in the field arrive where this computer's settings keep them.
    bool setField(const live::Field& field, const std::vector<live::TrackPair>& pairs,
                  const std::vector<live::HostInput>& inputs, juce::String& status);

    /// The lanes to drive, each on its instance's parameter as the set names it; a lane whose instance does not play,
    /// or whose parameter the instance does not have, waits. Message thread; driven again whenever what plays changes.
    void setLanes(std::vector<live::Lane> lanes);
    /// A parameter of an instance the engine plays, as a lane names it: its manifest and place there; none when
    /// it does not play.
    struct Target {
        const ParamManifest* manifest{nullptr};
        int at{-1};
        juce::AudioProcessor* plugin{nullptr};
    };
    Target targetOf(live::Instance instance, std::uint64_t owner, std::string_view param) const;

    /// Every level from Live, as `tree` has it now, for the field last set: the sends' knobs, a split group's fader
    /// on its bus, a field return's fader. Message thread; the runner ramps to them.
    void setSends(const live::Field& field, const live::Tree& tree);

    /// Drops the states kept for a track Live no longer has, or an insert the shape no longer has. Message thread.
    void forget(const live::Tree& tree, const live::Shape& shape);

    /// Frees the programs and Encoders the audio thread has let go of. Message thread.
    void tidy() { runner_.tidy(); }

    /// Every plugin's state, as the set keeps it.
    Json states() const;
    /// States the set gave back, into the plugins they belong to; one for a track not on a pair waits for it.
    void restore(const Json& states);

    /// The host's own part of the spatial mix's lateness: the loop device both ways, the device's buffer and the
    /// plan. Live's own part comes on top.
    double latencyMs() const { return latencyMs_; }

    /// The Encoder on the lowest pair a track is on; none while no track is.
    juce::AudioProcessor* firstEncoder() const;
    /// The Encoder of `track`, and the plugin of an insert; none when the engine plays no such thing.
    juce::AudioProcessor* encoderOf(live::TrackId track) const;
    juce::AudioProcessor* effectOf(std::uint64_t insert) const;
    /// The first insert of that kind, in the order of the slots; none while there is none.
    juce::AudioProcessor* firstEffect(live::Effect effect) const;
    /// Whether `p` is one of the plugins the engine plays now.
    bool holds(const juce::AudioProcessor* p) const;
    /// The Encoder of the track on `pair`; none when no track is on it.
    juce::AudioProcessor* encoder(int pair) const;
    /// The first Decoder, and the one with `id`; none when there is no such one.
    juce::AudioProcessor* decoder() const { return decoders_.empty() ? nullptr : decoders_.front().plugin.get(); }
    juce::AudioProcessor* decoderOf(std::uint64_t id) const;

private:
    struct Fx {  // an insert, and the Echo or Reverb that plays it
        live::Insert insert;
        std::shared_ptr<juce::AudioPluginInstance> plugin;
    };

    struct Voice {  // a source, and its Encoder
        live::TrackId track{0};
        int pair{-1};  ///< a track's pair of the loop device; -1 for an input of the host
        std::shared_ptr<juce::AudioPluginInstance> encoder;
    };

    struct Timed {
        live::TransportMessage message;
        std::int64_t stampNs{0};
    };
    struct TimedMidi {
        std::array<std::uint8_t, 3> bytes{};
        std::uint8_t size{0};
        bool live{false};  ///< from the host's own port
        std::int64_t stampNs{0};
    };

    bool open(juce::String& status);
    std::shared_ptr<juce::AudioPluginInstance> loadDecoder(juce::String& status);
    Runner::Setup runnerSetup() const;
    juce::String said() const;
    /// The routes out for the field `fieldLatency` samples late, from where the Decoder reports its outputs.
    std::vector<Runner::Route> routesFor(int fieldLatency) const;

    void audioDeviceIOCallbackWithContext(const float* const* inputs, int inputCount, float* const* outputs,
                                          int outputCount, int frames,
                                          const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message) override;
    void openMidi();
    /// The MIDI that arrived since the last block, onto the host's samples; Main's measured against Live's notes.
    void takeMidi(const float* const* inputs, int inputCount, int frames, std::int64_t now) noexcept;

    Settings s_;
    juce::AudioDeviceManager devices_;
    std::vector<Voice> voices_;          ///< in the order of their Encoder slots
    std::vector<Fx> effects_;            ///< in the order of their slots, after the Encoders
    live::Plan plan_;                    ///< the plan the runner was last given, its latencies aside
    std::vector<live::Insert> playing_;  ///< the inserts it plays, in the order of their slots
    live::FieldPlan fieldPlan_;          ///< the plan with its Encoders and inserts, for the send levels
    struct Dec {  // a Decoder, by the shape's id
        std::uint64_t id{0};
        std::shared_ptr<juce::AudioPluginInstance> plugin;
    };
    std::vector<Dec> decoders_;  ///< in the order of their slots, after the inserts
    Json waitingDecoders_ = Json::object();  ///< restored Decoder states, by id, for Decoders not playing
    Json waiting_ = Json::object();         ///< restored Encoder states, by track, for tracks not yet on a pair
    Json waitingEffects_ = Json::object();  ///< restored insert states, by insert, for inserts not yet playing
    int block_{64};
    Runner runner_;
    live::TransportProjector projector_;
    std::optional<live::Arrivals> arrivals_;
    SpscQueue<Timed, 256> queue_;  ///< full only while no block runs; a message then is dropped
    struct TimedSlot {
        live::SlotPlay report;
        std::int64_t stampNs{0};
    };
    SpscQueue<TimedSlot, 256> slots_;  ///< as `queue_`, for the session clips' reports
    live::ClipClock clips_;            ///< the audio thread's: where each track's session clip is
    std::vector<std::unique_ptr<juce::MidiInput>> midiInputs_;  ///< the system's, open
    std::unique_ptr<juce::MidiInput> port_;                     ///< the host's own, "bambi"
    const juce::MidiInput* livePort_{nullptr};  ///< the same, set before it starts and never again: the inputs' to read
    juce::MidiDeviceListConnection midiWatch_;
    juce::SpinLock midiPush_;  ///< between the inputs' threads, which may be several; the audio thread never takes it
    SpscQueue<TimedMidi, 1024> midi_;  ///< full only while no block runs; a message then is dropped
    live::MidiPending pending_;        ///< the audio thread's: MIDI on the host's samples, until its piece
    std::atomic<std::int64_t> liveMidi_{0};  ///< samples Live's MIDI is held back by
    std::atomic<bool> measuring_{false};
    bool wasMeasuring_{false};                ///< the audio thread's
    bool silence_{false};  ///< the audio thread's: MIDI was dropped, so every note is let go at the next block
    std::optional<live::LeadMeter> meter_;    ///< the audio thread's, made afresh as measuring starts
    SpscQueue<std::int64_t, 64> leads_;       ///< measured leads, in samples, to the message thread
    struct Program {  // what the runner was last given, to give it again with other routes or another device
        live::Compiled compiled;
        std::vector<juce::AudioProcessor*> slots;
        int levels{0};
        Runner::Keep keep;
    };
    Program program_;
    juce::String opened_;  ///< the device's name
    std::vector<const float*> inAt_;  ///< the callback's pointers into the device's inputs, one per channel
    juce::AudioBuffer<float> out_;    ///< the callback's output, copied to the device's
    int prepared_{0};      ///< the block the plugins are prepared for
    devices::Aggregate aggregate_;
    live::Row row_;           ///< the chosen devices' outputs: the Decoder's bus
    live::Row inputRow_;      ///< their inputs, after the loop device's on the input row
    std::vector<Runner::Route> routes_;  ///< the routes the runner was last given
    std::vector<live::Lane> lanes_;      ///< the lanes last given, driven where their instances play
    void drive();
    std::atomic<float> load_{0.0f};
    int loopChannels_{16};    ///< the loop device's channels the host opens, each way
    int inputChannels_{16};   ///< the input row: the loop device's, then the devices' inputs
    int outputChannels_{16};  ///< the loop device's, then the row
    int mainChannel_{0};      ///< the loop device's first pair: Live's Main in, the copy back to Live out
    double latencyMs_{0.0};
    int deviceLatency_{0};
    int outputLatency_{0};  ///< samples from the host's output to the speakers: the device's, and its buffer               ///< the loop device both ways, and its buffer
    std::atomic<bool> playable_{false};  ///< the device runs at the rate the plugins were prepared for
    std::int64_t faded_{0};              ///< samples of the fade-in played
};

}  // namespace bambi::livehost
