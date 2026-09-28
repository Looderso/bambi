// SPDX-License-Identifier: GPL-3.0-or-later
#include "PluginProcessor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <span>

#include "PluginEditor.h"
#include "bambi/encode/params.hpp"
#include "bambi/encode/pathparams.hpp"
#include "bambi/host/Layout.h"
#include "bambi/math/denormals.hpp"
#include "bambi/math/sh.hpp"
#include "bambi/math/vec3.hpp"
#include "bambi/path/generator.hpp"

namespace {}  // namespace

// =====================================================================================
BambiEncoderProcessor::BambiEncoderProcessor()
    : bambi::host::PluginProcessor(
          {bambi::Product::Encoder, bambi::encodeParams(), bambi::encodeMod(), 1},  // one region slot
          BusesProperties()
              .withInput("Input", juce::AudioChannelSet::stereo(), true)
              .withInput("Sidechain", juce::AudioChannelSet::stereo(), true)
              .withOutput("Ambisonic", juce::AudioChannelSet::ambisonic(kDefaultOrder), true),
          diag_) {
    //  A fresh instance starts on a real shape. TrajectoryState's generator parameters are zero
    //  until something fills them, which is a degenerate path.
    bambi::PluginState initial{bambi::encodeParams()};
    bambi::generatorDefaults(initial.trajectory.generator, initial.trajectory.genParams);
    begin(std::move(initial));  // the last line, as in every plugin
}

BambiEncoderProcessor::~BambiEncoderProcessor() {
    //  `describe`, `seedDynamic` and `onLinkTick` read this class's members: no tick after this
    stopLink();
    //  Unlisting a session that has no instances left is the node's job.
    //  No audio runs while the processor is destroyed, so every snapshot is ours to free.
}

// ---- buses ---------------------------------------------------------------------------

bool BambiEncoderProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    //  Main input: mono or stereo, in every format, its channels summed to mono. A 16-channel main
    //  input on a 16-channel REAPER track takes pins 1-16, so a send into track channels 3/4 lands
    //  inside it, where nothing reads it, and the sidechain bus, beginning at pin 17, hears nothing.
    //  Refused, JUCE's VST3 wrapper keeps the main input at 2 while still taking the output width,
    //  which puts the sidechain on pins 3/4. CLAP's port configurations never offer more than 2.
    //  The sidechain may be any width.
    const auto in = layouts.getMainInputChannelSet();
    bool ok = in.size() >= 1 && in.size() <= 2;

    //  Output: any width. The encoder runs at the highest order that fits -- (n+1)^2 channels, n up
    //  to kMaxHostOrder -- and leaves the channels beyond it silent. Accepting only exact (n+1)^2
    //  counts failed: a REAPER track has only even channel counts, so orders 2, 4, 6, 8 and 10
    //  (9, 25, 49, 81, 121 channels) could never be requested. One to three channels are order 0,
    //  omnidirectional. The host's name for the layout is never checked; only its width.
    const int outChannels = layouts.getMainOutputChannelSet().size();
    ok = ok && bambi::host::orderThatFits(outChannels) >= 0;

    appendLayoutLog(bambi::host::describeLayout(layouts) + (ok ? "   -> accepted" : "   -> refused"));
    return ok;
}

// ---- lifecycle -----------------------------------------------------------------------

void BambiEncoderProcessor::prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    const auto maxBlock = static_cast<std::size_t>(std::max(1, maximumExpectedSamplesPerBlock));
    monoIn_.assign(maxBlock, 0.0f);
    sideIn_.assign(maxBlock, 0.0f);
    monoSidechain_.assign(maxBlock, 0.0f);

    const int outChannels = getMainBusNumOutputChannels();
    encoderOrder_ = bambi::host::orderThatFits(outChannels);
    encoder_.prepare(std::max(0, encoderOrder_));
    encoderSide_.prepare(std::max(0, encoderOrder_));
    outPtrs_.assign(static_cast<std::size_t>(std::max(1, outChannels)), nullptr);
    segmentPtrs_.assign(outPtrs_.size(), nullptr);

    //  The energy picture's summary: what leaves, which is all an encoder has. One hop of it
    //  interleaved, since that is what a covariance window takes and the output is planar.
    prepareEnergy(encoderOrder_, sampleRate_, false);
    leaving_.assign(static_cast<std::size_t>(kControlHop) *
                        static_cast<std::size_t>(std::max(1, bambi::numChannels(std::max(0, encoderOrder_)))),
                    0.0f);

    features_.prepare(sampleRate_, 2048, kControlHop);
    sidechainFeatures_.prepare(sampleRate_, 2048, kControlHop);
    modulation().prepare(sampleRate_);
    modulation().usePatch(snapshot().patch);
    setLoadRate(sampleRate_);

    grid_.forget();
    resetEngine(0, false);
    transport_.forget();

    diag_.sampleRate.store(sampleRate_);
    diag_.blockSize.store(maximumExpectedSamplesPerBlock);
    diag_.mainInputChannels.store(getMainBusNumInputChannels());
    diag_.mainBusChannels.store(getMainBusNumInputChannels());
    diag_.sidechainChannels.store(getBusCount(true) > 1 ? getChannelCountOfBus(true, 1) : 0);
    diag_.sidechainBusChannels.store(getBusCount(true) > 1 ? getChannelCountOfBus(true, 1) : 0);
    diag_.outputChannels.store(outChannels);
    diag_.order.store(encoderOrder_);
    linkOrder_.store(std::max(0, encoderOrder_), std::memory_order_relaxed);
    link().markStaticDirty();
    diag_.prepares.fetch_add(1, std::memory_order_relaxed);
    appendLayoutLog("prepared at " + juce::String(sampleRate_, 0) +
                    " Hz:  " + bambi::host::describeLayout(getBusesLayout()));
}

void BambiEncoderProcessor::resetEngine(std::int64_t timelineSample, bool fromTransport) noexcept {
    features_.reset();
    sidechainFeatures_.reset();
    resetEnergy();

    //  From the transport -- a start, a locate, a loop's jump -- what is set to continue carries on: an LFO,
    //  and with rates.retrigger the source's place on its path and the placement's integrated turn. A bounce
    //  then no longer repeats what was heard; that is the user's choice. Preparing restarts all.
    if (fromTransport) {
        modulation().restartTransport();
        control_.restartTransport(sharedValue(ratesRetriggerAt(), 0.0f) > 0.5f);
    } else {
        modulation().reset();
        control_.reset();
    }

    //  The control grid is pinned to the timeline: playback and a bounce that start at the same
    //  place step the modulation at the same samples, whatever block sizes the host uses for
    //  each. The step at the reset point itself is forced by the grid.
    grid_.restartAt(timelineSample, kControlHop);  // and the notes carried from before the jump go with it
    diag_.resets.fetch_add(1, std::memory_order_relaxed);
}

// ---- editing the patch ------------------------------------------------------------------

// ---- engine snapshots: state reaching the audio thread without a lock -----------------

std::unique_ptr<BambiEncoderProcessor::EngineSnapshot> BambiEncoderProcessor::newSnapshot(
    const bambi::PluginState& st) const {
    //  What the encoder carries beyond the shared cargo: its path, built, and where that is centred.
    auto snapshot = std::make_unique<EncoderSnapshot>();
    snapshot->shape = shapeAsSet(st.trajectory);  // a parametric path's settings are host parameters
    snapshot->trajectory.build(snapshot->shape);
    snapshot->centre = bambi::pathCentre(snapshot->trajectory.points());
    return snapshot;
}

bambi::TrajectoryState BambiEncoderProcessor::shapeAsSet(bambi::TrajectoryState shape) const {
    std::array<float, bambi::kMaxParams> values{};
    for (int i = 0; i < bambi::kNumEncoderParams; ++i)
        if (auto* p = hostParameter(static_cast<bambi::ParamId>(i)))
            values[static_cast<std::size_t>(i)] = p->convertFrom0to1(p->getValue());
    bambi::pathSettingsFrom(shape, values);
    return shape;
}

void BambiEncoderProcessor::committing(const EngineSnapshot& next) {
    //  under the document's lock: the message thread's copy of the path moves with the document
    documentPath_ = static_cast<const EncoderSnapshot&>(next).trajectory;
    documentShape_ = static_cast<const EncoderSnapshot&>(next).shape;
}

void BambiEncoderProcessor::snapshotAdopted(const EngineSnapshot& next) noexcept {
    if (next.snap) control_.requestSnap();  // a state load jumps the source onto the new path; an edit moves it there
}

// ---- audio ---------------------------------------------------------------------------

void BambiEncoderProcessor::renderBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    bambi::ScopedFlushDenormals flushDenormals;  // the render tools' environment too
    const int numSamples = buffer.getNumSamples();
    diag_.blockSamples.store(numSamples, std::memory_order_relaxed);
    if (numSamples <= 0) return;
    const auto n = static_cast<std::size_t>(numSamples);

    //  A state load or an edit posted since the last block takes effect here, at the boundary.
    adoptPendingSnapshot();

    //  MIDI, counted for the readout. The notes themselves reach the envelope triggers in the
    //  segment loop below, each at the control step it belongs to.
    for (const auto metadata : midi) {
        const auto message = metadata.getMessage();
        diag_.midiEvents.fetch_add(1, std::memory_order_relaxed);
        if (message.isNoteOn()) diag_.lastNote.store(message.getNoteNumber(), std::memory_order_relaxed);
    }
    catchLearnedNote(midi);

    //  Parameters: one lock-free copy per block, handed to the engine without allocating.
    intakeParameters();
    modulation().setParameters(parameterValues());
    //  How long each Level takes to fall. Milliseconds at the boundary, seconds inside.
    features_.setLevelRelease(static_cast<double>(sharedValue(levelReleaseAt(), 200.0f)) * 0.001);
    sidechainFeatures_.setLevelRelease(static_cast<double>(sharedValue(scLevelReleaseAt(), 200.0f)) * 0.001);

    if (n > monoIn_.size()) {
        //  A host exceeding its own announced maximum. Silence, never a resize on this thread.
        buffer.clear();
        grid_.pass(numSamples, kControlHop, midi);
        diag_.notesDropped.store(grid_.notesDropped(), std::memory_order_relaxed);
        return;
    }

    //  Inputs are summed to mono before anything is written: in-place hosts hand over the same
    //  memory for input and output channels.
    const auto sumToMono = [n](const juce::AudioBuffer<float>& bus, std::vector<float>& mono) {
        std::fill_n(mono.begin(), n, 0.0f);
        //  The main input is mono or stereo and nothing wider (isBusesLayoutSupported); the bound is
        //  what keeps the source the first pair if a host ever hands over more.
        const int channels = std::min(2, bus.getNumChannels());
        if (channels == 0) return 0.0f;
        //  Mean of those channels, accumulated channel by channel: the same downmix, in the same
        //  order of operations, as the core's AudioBuffer::mono() on a stereo file.
        const float k = 1.0f / static_cast<float>(channels);
        for (int c = 0; c < channels; ++c) {
            const float* x = bus.getReadPointer(c);
            for (std::size_t i = 0; i < n; ++i) mono[i] += x[i] * k;
        }
        float peak = 0.0f;
        for (std::size_t i = 0; i < n; ++i) peak = std::max(peak, std::abs(mono[i]));
        return peak;
    };

    /*  The input's side, (L - R) / 2, taken with the mid and for the same reason: before anything is
        written. The two encoders are always fed these two, whatever the input mode; a mono
        bus has no side, and every mode is then `sum`. */
    {
        const auto& main = getBusBuffer(buffer, true, 0);
        std::fill_n(sideIn_.begin(), n, 0.0f);
        if (main.getNumChannels() >= 2) {
            const float* l = main.getReadPointer(0);
            const float* r = main.getReadPointer(1);
            for (std::size_t i = 0; i < n; ++i) sideIn_[i] = (l[i] - r[i]) * 0.5f;
        }
    }
    diag_.inputPeak.store(sumToMono(getBusBuffer(buffer, true, 0), monoIn_), std::memory_order_relaxed);
    sidechainActive_ = getBusCount(true) > 1 && getBus(true, 1)->isEnabled() && getChannelCountOfBus(true, 1) > 0;
    diag_.sidechainPeak.store(sidechainActive_ ? sumToMono(getBusBuffer(buffer, true, 1), monoSidechain_) : 0.0f,
                              std::memory_order_relaxed);

    //  Live channel counts, every block. A host may change a layout without calling
    //  prepareToPlay again, and the readout has to show what is actually arriving.
    const int liveOut = getBusBuffer(buffer, false, 0).getNumChannels();
    diag_.mainInputChannels.store(getBusBuffer(buffer, true, 0).getNumChannels(), std::memory_order_relaxed);
    diag_.mainBusChannels.store(getChannelCountOfBus(true, 0), std::memory_order_relaxed);
    //  What arrives, not what the layout declares: reading the declaration told us a bus was 16 channels
    //  wide while the negotiated layout said 2, and only the arriving width says what can be read.
    diag_.sidechainChannels.store(sidechainActive_ ? getBusBuffer(buffer, true, 1).getNumChannels() : 0,
                                  std::memory_order_relaxed);
    diag_.sidechainBusChannels.store(getBusCount(true) > 1 ? getChannelCountOfBus(true, 1) : 0,
                                     std::memory_order_relaxed);
    diag_.outputChannels.store(liveOut, std::memory_order_relaxed);
    diag_.order.store(bambi::host::orderThatFits(liveOut), std::memory_order_relaxed);

    /*  Where the host is, and whether it jumped. The decision -- and the long reason the tolerance
        exists -- is the shared one: two plugins that disagreed about where a timeline
        jumped would put two different renders in the same bounce. */
    const auto tp = transport_.observe(getPlayHead(), numSamples,
                                       static_cast<std::int64_t>(monoIn_.size()) + kTimelineSlack, sampleRate_);
    const bool playing = tp.playing;
    const std::int64_t timeline = tp.timeline;
    const double ppq = tp.ppq, bpm = tp.bpm;

    diag_.playing.store(playing, std::memory_order_relaxed);
    diag_.transportKnown.store(timeline >= 0, std::memory_order_relaxed);
    diag_.bpm.store(bpm,
                    std::memory_order_relaxed);  // the shared readout: a reader of it must not get the default here
    diag_.timeInSamples.store(timeline, std::memory_order_relaxed);
    diag_.nonRealtime.store(isNonRealtime(), std::memory_order_relaxed);
    if (tracing_.load(std::memory_order_relaxed))  // diagnostics: when this block arrived (FrameTrace)
        blockTrace_.push({kTraceBlock, bambi::linkNowMicros(), static_cast<std::int64_t>(numSamples),
                          static_cast<std::int64_t>(timeline)});

    if (tp.restart) resetEngine(tp.start, true);

    //  Output. Clearing first is also what silences the channels beyond the encoded order: on an
    //  in-place host they still hold input audio.
    auto out = getBusBuffer(buffer, false, 0);
    const int outChannels = out.getNumChannels();
    out.clear();
    if (encoderOrder_ < 0 || bambi::host::orderThatFits(outChannels) != encoderOrder_ ||
        static_cast<std::size_t>(outChannels) > outPtrs_.size()) {
        //  The layout changed without a prepareToPlay. Silence rather than a guess.
        grid_.pass(numSamples, kControlHop, midi);
        diag_.notesDropped.store(grid_.notesDropped(), std::memory_order_relaxed);
        diag_.layoutMismatches.fetch_add(1, std::memory_order_relaxed);
        diag_.outputPeak.store(0.0f, std::memory_order_relaxed);
        return;
    }
    const int encoded = bambi::numChannels(encoderOrder_);
    for (int c = 0; c < encoded; ++c) outPtrs_[static_cast<std::size_t>(c)] = out.getWritePointer(c);

    const double dt = static_cast<double>(kControlHop) / sampleRate_;
    /*  Every note at or before a step and after the previous one reaches the envelopes at that step,
        carried over from the last block if it fell after its last one: where the host cuts blocks
        never moves a trigger. The grid is every plugin's. */
    const bool gathering = gatheringEnergy();  // once a block: a window has the picture on
    grid_.hear(ppq, bpm, playing, sampleRate_);
    grid_.walk(
        numSamples, kControlHop, midi, modulation(),
        [&](std::size_t) { controlStep(grid_.ppqNow(), bpm, playing, dt); },
        [&](std::size_t pos, std::size_t m) {
            //  Input trim, applied where both the detectors and the encoder see it: trimming after
            //  the features would leave a quiet take unable to open its own gate, which is the
            //  whole use for the control.
            if (trimDb_ != 0.0) {
                const auto g = static_cast<float>(std::pow(10.0, trimDb_ / 20.0));
                for (std::size_t i = 0; i < m; ++i) monoIn_[pos + i] *= g;
                for (std::size_t i = 0; i < m; ++i)  // the side is the same input: trimmed with it
                    sideIn_[pos + i] *= g;
            }
            features_.process(monoIn_.data() + pos, static_cast<int>(m));
            if (sidechainActive_) sidechainFeatures_.process(monoSidechain_.data() + pos, static_cast<int>(m));

            for (int ch = 0; ch < encoded; ++ch)
                segmentPtrs_[static_cast<std::size_t>(ch)] = outPtrs_[static_cast<std::size_t>(ch)] + pos;
            encoder_.process(monoIn_.data() + pos,
                             std::span<float* const>(segmentPtrs_.data(), static_cast<std::size_t>(encoded)),
                             static_cast<int>(m));
            //  Aimed at nothing in `sum`, and then not run at all: adding exact zeros is still
            //  adding, and the default path is the one every golden hashes.
            if (!encoderSide_.silent())
                encoderSide_.process(sideIn_.data() + pos,
                                     std::span<float* const>(segmentPtrs_.data(), static_cast<std::size_t>(encoded)),
                                     static_cast<int>(m));
            if (gathering) {
                //  planar to interleaved, this segment only: at most one hop, which `leaving_` holds
                const auto channels = static_cast<std::size_t>(encoded);
                for (std::size_t c = 0; c < channels; ++c) {
                    const float* from = segmentPtrs_[c];
                    for (std::size_t i = 0; i < m; ++i) leaving_[i * channels + c] = from[i];
                }
                gatherLeaving(leaving_.data(), static_cast<int>(m));
            }
        });
    if (gathering) publishEnergy();

    diag_.notesDropped.store(grid_.notesDropped(), std::memory_order_relaxed);
    diag_.controlPhase.store(grid_.phase(), std::memory_order_relaxed);
    diag_.outputPeak.store(out.getMagnitude(0, numSamples), std::memory_order_relaxed);
}

void BambiEncoderProcessor::zeroRotation(int axis) noexcept {
    if (axis >= 0 && axis < 4)  // 3 is the distance travelled, displace's turn
        zeroTurns_.fetch_or(1 << axis, std::memory_order_relaxed);
}

void BambiEncoderProcessor::controlStep(double ppq, double bpm, bool playing, double dt) noexcept {
    std::array<double, bambi::kNumFeatures> self{}, side{};
    for (int i = 0; i < bambi::kNumFeatures; ++i) {
        self[static_cast<std::size_t>(i)] = features_.value(static_cast<bambi::Feature>(i));
        side[static_cast<std::size_t>(i)] = sidechainFeatures_.value(static_cast<bambi::Feature>(i));
    }
    bambi::ControlInput in;
    in.self = self;
    if (sidechainActive_) in.sidechain = side;
    in.transport.playing = playing;
    in.transport.bpm = bpm;
    in.transport.ppq = ppq;
    in.dt = dt;
    in.path = &current().trajectory;
    in.shape = &current().shape;  // a parametric path is rebuilt from its settings as the engine leaves them
    in.centre = current().centre;
    in.zeroTurns = zeroTurns_.exchange(0, std::memory_order_relaxed);  // an angle set back to zero by hand
    in.zeroRegionTurns = takeRegionZeros(0);
    in.region = &current().regions[0];
    in.order = std::max(0, encoderOrder_);
    in.stereoInput = getMainBusNumInputChannels() >= 2;

    //  What happens in a step is bambi::EncoderControl's, which bambi-render calls too: the goldens
    //  hash this step, not a copy of it.
    const bambi::ControlFrame frame = control_.step(modulation(), parameterValues(), in);
    bambi::applyFrame(encoder_, encoderSide_, frame);
    trimDb_ = frame.trimDb;
    publishSources();  // for a source's settings: what it is sending now
    publishTurn(0, control_.regionRotation());

    const double s = frame.s;
    const double speed = frame.speed;
    const bambi::Vec3 p = frame.position;
    const bambi::PathTransform& xf = frame.placement;

    //  Where the source is, for every other instance's scene: a wait-free copy into
    //  process-local memory. linkTick publishes it; nothing here touches the bus.
    bambi::LinkDynamic published;
    published.s = static_cast<float>(s);
    published.x = static_cast<float>(p.x);
    published.y = static_cast<float>(p.y);
    published.z = static_cast<float>(p.z);
    //  a stereo input's two points, resolved as the position is: what a scene draws them from
    published.inputMode = static_cast<std::uint8_t>(frame.inputMode);
    published.leftX = static_cast<float>(frame.left.x);
    published.leftY = static_cast<float>(frame.left.y);
    published.leftZ = static_cast<float>(frame.left.z);
    published.rightX = static_cast<float>(frame.right.x);
    published.rightY = static_cast<float>(frame.right.y);
    published.rightZ = static_cast<float>(frame.right.z);
    published.level = static_cast<float>(self[static_cast<std::size_t>(bambi::Feature::Level)]);
    published.yawRad = static_cast<float>(xf.yawRad);
    published.pitchRad = static_cast<float>(xf.pitchRad);
    published.rollRad = static_cast<float>(xf.rollRad);
    published.extent = static_cast<float>(xf.extent);
    //  the region as resolved, turn included, for every other instance's scene
    published.regionCount = 1;
    published.regions[0].set(frame.region);
    publishEngine(published);  // every value as the engine has it, the sources' outputs, and when
    link().publish(published);

    diag_.speed.store(static_cast<float>(speed), std::memory_order_relaxed);
    diag_.azimuthDeg.store(static_cast<float>(bambi::azimuth(p) * bambi::kRad2Deg), std::memory_order_relaxed);
    diag_.elevationDeg.store(static_cast<float>(bambi::elevation(p) * bambi::kRad2Deg), std::memory_order_relaxed);
}

// ---- editor --------------------------------------------------------------------------

juce::AudioProcessorEditor* BambiEncoderProcessor::createEditor() { return new BambiEncoderEditor(*this); }

// ---- link bus ------------------------------------------------------------------------
//
//  Joining, publishing, and taking in another window's edits are bambi::host::LinkNode's now.
//  What is left here is what only the encoder can answer.

void BambiEncoderProcessor::onLinkTick() {
    if (tracing_.load(std::memory_order_relaxed) && tickTrace_.size() < 16384)
        tickTrace_.push_back({kTraceTick, bambi::linkNowMicros(), 0, -1});

    /*  A path setting moved by the host -- automation, a drag -- moves no document, so the path every scene
        draws is rebuilt here when its parameters no longer say what it was built from. */
    bambi::TrajectoryState was;
    {
        const std::lock_guard<std::mutex> lock(committedMutex());
        was = documentShape_;
    }
    if (was.kind != bambi::TrajectoryKind::Parametric) return;
    const auto now = shapeAsSet(was);
    if (now.genParams == was.genParams) return;
    bambi::Trajectory rebuilt;
    rebuilt.build(now);
    {
        const std::lock_guard<std::mutex> lock(committedMutex());
        if (!(documentShape_ == was)) return;  // a commit landed meanwhile, with a path of its own: that one stands
        documentPath_ = std::move(rebuilt);
        documentShape_ = now;
    }
    link().markStaticDirty();
}

void BambiEncoderProcessor::describe(bambi::LinkStatic& st) const {
    //  The message thread's own copy of the path; the audio thread's is never read here.
    const std::lock_guard<std::mutex> lock(committedMutex());
    st.setPath(documentPath_);
}

void BambiEncoderProcessor::seedDynamic(bambi::LinkDynamic& seed) const {
    //  Seed the published position with the start of the path, so a stopped transport does not
    //  leave the source drawn at the front of the sphere until audio runs.
    bambi::Vec3 start{1.0, 0.0, 0.0};
    {
        const std::lock_guard<std::mutex> lock(committedMutex());
        if (!documentPath_.empty()) start = documentPath_.eval(0.0);
    }
    seed.x = static_cast<float>(start.x);
    seed.y = static_cast<float>(start.y);
    seed.z = static_cast<float>(start.z);
}

void BambiEncoderProcessor::setTracing(bool on) noexcept {
    tracing_.store(on, std::memory_order_relaxed);
    if (!on) tickTrace_.clear();
}

void BambiEncoderProcessor::drainTrace(std::vector<TraceEvent>& out) {
    TraceEvent event;
    while (blockTrace_.pop(event)) out.push_back(event);
    out.insert(out.end(), tickTrace_.begin(), tickTrace_.end());
    tickTrace_.clear();
}

// ---- CLAP ----------------------------------------------------------------------------

int BambiEncoderProcessor::clapAmbisonicOrderForBus(bool isInput, int busIndex) {
    if (isInput || busIndex != 0) return -1;
    return bambi::host::orderForChannels(getChannelCountOfBus(false, 0));
}

uint32_t BambiEncoderProcessor::clapAudioPortsConfigCount() { return static_cast<uint32_t>(kMaxHostOrder); }

bool BambiEncoderProcessor::clapAudioPortsConfigGet(uint32_t index, clap_audio_ports_config* config) {
    if (config == nullptr || index >= static_cast<uint32_t>(kMaxHostOrder)) return false;
    const int order = static_cast<int>(index) + 1;
    const int channels = bambi::numChannels(order);
    //  The id is the position: a host passing either one as if it were the other still selects
    //  the same configuration.
    config->id = static_cast<clap_id>(index);
    std::snprintf(config->name, sizeof config->name, "Ambisonic order %d (%d channels)", order, channels);
    config->input_port_count = 2;
    config->output_port_count = 1;
    config->has_main_input = true;
    config->main_input_channel_count = 2;
    config->main_input_port_type = CLAP_PORT_STEREO;
    config->has_main_output = true;
    config->main_output_channel_count = static_cast<uint32_t>(channels);
    config->main_output_port_type = CLAP_PORT_AMBISONIC;
    return true;
}

bool BambiEncoderProcessor::clapAudioPortsConfigSelect(clap_id configId) {
    const int order = static_cast<int>(configId) + 1;  // id == position, see above
    if (order < 1 || order > kMaxHostOrder) return false;
    auto layout = getBusesLayout();
    const int channels = bambi::numChannels(order);
    layout.getChannelSet(false, 0) =
        order <= 7 ? juce::AudioChannelSet::ambisonic(order) : juce::AudioChannelSet::discreteChannels(channels);
    return setBusesLayout(layout);
}

// ---- layout log ----------------------------------------------------------------------

void BambiEncoderProcessor::appendLayoutLog(const juce::String& entry) const {
    const std::lock_guard<std::mutex> lock(layoutLogMutex_);
    //  Hosts ask the same question many times over; repeats are counted, not listed.
    if (entry == lastLayoutEntry_ && !layoutLog_.isEmpty()) {
        ++lastLayoutRepeats_;
        layoutLog_.set(0, entry + "   x" + juce::String(lastLayoutRepeats_ + 1));
        return;
    }
    lastLayoutEntry_ = entry;
    lastLayoutRepeats_ = 0;
    layoutLog_.insert(0, entry);
    while (layoutLog_.size() > 12) layoutLog_.remove(layoutLog_.size() - 1);
}

juce::StringArray BambiEncoderProcessor::layoutLog() const {
    const std::lock_guard<std::mutex> lock(layoutLogMutex_);
    return layoutLog_;
}

// =====================================================================================
