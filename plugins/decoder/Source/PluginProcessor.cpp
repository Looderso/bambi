// SPDX-License-Identifier: GPL-3.0-or-later
#include "PluginProcessor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "BinaryData.h"
#include "PluginEditor.h"
#include "bambi/decode/headmodelfile.hpp"
#include "bambi/decode/measuredhead.hpp"
#include "bambi/decode/sofafile.hpp"
#include "bambi/dsp/resample.hpp"
#include "bambi/host/ClapPorts.h"
#include "bambi/math/denormals.hpp"
#include "bambi/math/sh.hpp"

namespace {
bambi::ParamId decoderParam(const char* key) { return static_cast<bambi::ParamId>(bambi::decodeParams().byKey(key)); }

/// The layout as the design should see it: with the distances levelled when compensation is off.
bambi::Layout layoutFor(const bambi::DecoderState& d) {
    bambi::Layout l;
    l.speakers = d.layout;
    if (!d.distanceCompensation)
        for (bambi::Loudspeaker& s : l.speakers) s.radiusM = 1.0;
    return l;
}

/// The channels a layout's real loudspeakers play on, counted from 1; a filter matrix plays on 1 to its outputs.
std::vector<int> layoutChannels(const bambi::DecoderState& d) {
    std::vector<int> out;
    if (!d.matrixFile.empty()) {
        for (int c = 1; c <= d.matrixOutputs; ++c) out.push_back(c);
        return out;
    }
    for (const bambi::Loudspeaker& s : d.layout)
        if (!s.imaginary) out.push_back(s.channel);
    std::sort(out.begin(), out.end());
    return out;
}

/// What a bus's description says: the same line the layout log keeps.
juce::String busLine(const juce::AudioProcessor::BusesLayout& layouts) {
    return "in " + juce::String(layouts.getMainInputChannelSet().size()) + "  |  out " +
           juce::String(layouts.getMainOutputChannelSet().size());
}
}  // namespace

BambiDecoderProcessor::BambiDecoderProcessor()
    : bambi::host::PluginProcessor({.product = bambi::Product::Decoder,
                                    .params = bambi::decodeParams(),
                                    .mod = bambi::decodeMod(),
                                    .regionSlots = 1},
                                   BusesProperties()
                                       .withInput("Ambisonic", juce::AudioChannelSet::ambisonic(3), true)
                                       .withOutput("Output", juce::AudioChannelSet::stereo(), true),
                                   diag_) {
    std::string error;
    modelLoaded_ = bambi::parseHeadModel(
                       std::string_view(BinaryData::head_json, static_cast<std::size_t>(BinaryData::head_jsonSize)),
                       models_[0], &error) &&
                   bambi::parseHeadModel(
                       std::string_view(BinaryData::ku100_json, static_cast<std::size_t>(BinaryData::ku100_jsonSize)),
                       models_[1], &error);
    jassert(modelLoaded_);
    osc_.addListener(this);
    begin(bambi::PluginState{bambi::decodeParams()});
}

BambiDecoderProcessor::~BambiDecoderProcessor() {
    stopLink();
    osc_.removeListener(this);
    osc_.disconnect();
    helper_.terminate();
}

// ---- buses ---------------------------------------------------------------------------------

bool BambiDecoderProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    //  The input is the field, at the highest order that fits its width: REAPER offers only even channel counts,
    //  so orders 2, 4, 6 and 8 arrive with spare channels. The output is as wide as the host gives, two at least
    //  for the headphones' pair; every output is a set of channels on it.
    const bool ok = bambi::host::orderThatFits(layouts.getMainInputChannelSet().size()) >= 0 &&
                    layouts.getMainOutputChannelSet().size() >= 2 &&
                    layouts.getMainOutputChannelSet().size() <= kMaxOutputs;
    appendLayoutLog(busLine(layouts) + (ok ? "   -> accepted" : "   -> refused"));
    return ok;
}

int BambiDecoderProcessor::clapAmbisonicOrderForBus(bool isInput, int busIndex) {
    if (!isInput || busIndex != 0) return -1;
    return bambi::host::orderForChannels(getChannelCountOfBus(true, 0));
}

uint32_t BambiDecoderProcessor::clapAudioPortsConfigCount() { return static_cast<uint32_t>(kMaxHostOrder); }

bool BambiDecoderProcessor::clapAudioPortsConfigGet(uint32_t index, clap_audio_ports_config* config) {
    if (config == nullptr || index >= static_cast<uint32_t>(kMaxHostOrder)) return false;
    const int order = static_cast<int>(index) + 1;
    const int channels = bambi::numChannels(order);
    const int outChannels = std::max(2, getChannelCountOfBus(false, 0));
    //  The id is the position: a host passing either one as if it were the other still selects the same one.
    config->id = static_cast<clap_id>(index);
    std::snprintf(config->name, sizeof config->name, "Ambisonic order %d in (%d channels), %d out", order, channels,
                  outChannels);
    config->input_port_count = 1;
    config->output_port_count = 1;
    config->has_main_input = true;
    config->main_input_channel_count = static_cast<uint32_t>(channels);
    config->main_input_port_type = CLAP_PORT_AMBISONIC;
    config->has_main_output = true;
    config->main_output_channel_count = static_cast<uint32_t>(outChannels);
    config->main_output_port_type = outChannels == 2 ? CLAP_PORT_STEREO : nullptr;
    return true;
}

bool BambiDecoderProcessor::clapAudioPortsConfigSelect(clap_id configId) {
    const int order = static_cast<int>(configId) + 1;  // id == position, see above
    if (order < 1 || order > kMaxHostOrder) return false;
    auto layout = getBusesLayout();
    layout.getChannelSet(true, 0) = bambi::host::ambisonicSet(order);
    return setBusesLayout(layout);
}

void BambiDecoderProcessor::appendLayoutLog(const juce::String& entry) const {
    const std::lock_guard<std::mutex> lock(logMutex_);
    layoutLog_.add(entry);
    while (layoutLog_.size() > 64) layoutLog_.remove(0);
}

juce::StringArray BambiDecoderProcessor::layoutLog() const {
    const std::lock_guard<std::mutex> lock(logMutex_);
    return layoutLog_;
}

// ---- lifecycle -----------------------------------------------------------------------------

void BambiDecoderProcessor::prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) {
    //  The committed state first and on its own: committed() takes the document's lock itself, and the work lock
    //  never nests inside it the other way round than in committing().
    const bambi::DecoderState state = document().committed().decoder;
    const std::lock_guard<std::mutex> work(workMutex_);
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    maxBlock_ = std::max(1, maximumExpectedSamplesPerBlock);
    engineOrder_ = bambi::host::orderThatFits(getChannelCountOfBus(true, 0));
    diag_.sampleRate.store(sampleRate_, std::memory_order_relaxed);
    diag_.blockSize.store(maxBlock_, std::memory_order_relaxed);
    modulation().prepare(sampleRate_);
    setLoadRate(sampleRate_);
    setLatencySamples(kPartition);
    if (engineOrder_ < 0 || !modelLoaded_) return;

    //  Everything that allocates happens here and in build(), never on the audio thread.
    bambi::FilterDesign design;
    design.order = engineOrder_;
    design.sampleRate = sampleRate_;
    chosenHead_ = headIndex(state);
    binaural_.prepare(headModel(), design, kPartition);
    speakers_.prepare(engineOrder_, kMaxOutputs, sampleRate_);
    virtual_.prepare(headModel(), engineOrder_, sampleRate_, kPartition);
    turn_.prepare(engineOrder_, sampleRate_);
    field_.prepare(maxBlock_, engineOrder_);
    const auto channels = static_cast<std::size_t>(bambi::numChannels(engineOrder_));
    turned_.assign(static_cast<std::size_t>(maxBlock_) * channels, 0.0f);
    stereo_.assign(static_cast<std::size_t>(maxBlock_) * 2, 0.0f);
    speakersOut_.assign(static_cast<std::size_t>(maxBlock_) * static_cast<std::size_t>(kMaxOutputs), 0.0f);
    gainRamp_.assign(static_cast<std::size_t>(maxBlock_), 1.0f);
    gainCoefficient_ = 1.0f - std::exp(-1.0f / (0.01f * static_cast<float>(sampleRate_)));  // 10 ms
    gainNow_ = 1.0f;
    trackerSeen_ = false;

    //  Built before the first block, so what plays is a function of the state alone.
    headBuilt_ = false;
    builtOrder_ = -1;
    builtMatrixOrder_ = -1;
    matrixPlays_.store(false, std::memory_order_relaxed);
    build(state);
    connectTracker(state);
}

std::unique_ptr<bambi::host::PluginProcessor::EngineSnapshot> BambiDecoderProcessor::newSnapshot(
    const bambi::PluginState& st) const {
    auto snap = std::make_unique<DecoderSnapshot>();
    snap->decoder = st.decoder;
    return snap;
}

void BambiDecoderProcessor::committing(const EngineSnapshot& next) {
    const auto& d = static_cast<const DecoderSnapshot&>(next).decoder;
    const std::lock_guard<std::mutex> work(workMutex_);
    if (engineOrder_ >= 0 && modelLoaded_) build(d);
    connectTracker(d);
}

void BambiDecoderProcessor::snapshotAdopted(const EngineSnapshot& next) noexcept {
    if (next.snap) modulation().settle();
}

int BambiDecoderProcessor::headIndex(const bambi::DecoderState& d) {
    if (d.hrtfFileHead.text != fileHeadText_) {
        fileHeadText_ = d.hrtfFileHead.text;
        std::string error;
        fileLoaded_ = fileHeadText_ != nullptr && bambi::parseHeadModel(*fileHeadText_, fileModel_, &error);
    }
    if (d.hrtf == 2) return fileLoaded_ ? 2 : 0;  // a file head the session lost plays the model, not silence
    return d.hrtf == 1 ? 1 : 0;
}

bool BambiDecoderProcessor::loadSofaFile(const juce::File& file, juce::String* error) {
    bambi::MeasuredHead measured;
    std::string why;
    if (!bambi::readSofa(file.getFullPathName().toStdString(), measured, &why)) {
        if (error) *error = juce::String(why);
        return false;
    }
    const bambi::HeadFit fit;  // the shipped KU100's: order 20 on the 0.2-ERB axis
    bambi::HeadModel head;
    if (!bambi::fitHeadModel(measured, fit, head, &why)) {
        if (error) *error = juce::String(why);
        return false;
    }
    const bambi::HeadFitError e = bambi::headFitError(measured, head, 2000.0, 16000.0);
    sofaNote_ = juce::String(measured.directions.size()) + " directions, " + juce::String(measured.taps) + " taps at " +
                juce::String(measured.sampleRate / 1000.0, 1) + " kHz; fitted within " + juce::String(e.cllDb, 2) +
                " dB level, " + juce::String(e.ildDb, 2) + " dB difference, " + juce::String(e.itdUs, 0) + " us";
    auto text = std::make_shared<const std::string>(bambi::headModelJson(head));
    const std::string path = file.getFullPathName().toStdString();
    editDecoder("load sofa file", [text, path](bambi::DecoderState& s) {
        s.hrtf = 2;
        s.hrtfFile = path;
        s.hrtfFileHead.text = text;
    });
    return true;
}

void BambiDecoderProcessor::build(const bambi::DecoderState& d) {
    //  Another head: the chain's synthesised head is replaced between builds, and the sliders are built on it anew.
    if (headIndex(d) != chosenHead_) {
        chosenHead_ = headIndex(d);
        binaural_.setModel(headModel());
        virtual_.setModel(headModel());
        headBuilt_ = false;
    }
    //  The listener's two sliders: the binaural filters, and the array on headphones' at the field's order.
    const auto differs = [](double a, double b) { return std::abs(a - b) > 1e-9; };
    if (!headBuilt_ || differs(builtShift_, d.earShift) || differs(builtSize_, d.headSize)) {
        binaural_.request(d.earShift, d.headSize);
        binaural_.work();
        virtual_.buildHead(d.earShift, d.headSize);
        builtShift_ = d.earShift;
        builtSize_ = d.headSize;
        headBuilt_ = true;
    }
    //  A decoder as filters, when the state names one: it replaces the layout's design below.
    if (d.matrixFile != builtMatrixFile_ || d.matrixNormalization != builtMatrixNormalization_ ||
        builtMatrixOrder_ != engineOrder_) {
        const bool leaving = !builtMatrixFile_.empty() && d.matrixFile.empty();
        builtMatrixFile_ = d.matrixFile;
        builtMatrixNormalization_ = d.matrixNormalization;
        builtMatrixOrder_ = engineOrder_;
        buildMatrix(d);
        if (leaving) builtOrder_ = -1;  // the layout's design decides the outputs again
    }
    //  The layout's decoder, and the same layout heard on headphones.
    if (d.matrixFile.empty() && (builtOrder_ != engineOrder_ || builtLayout_ != d.layout ||
                                 builtWeighting_ != d.weighting || builtDistance_ != d.distanceCompensation)) {
        builtOrder_ = engineOrder_;
        builtLayout_ = d.layout;
        builtWeighting_ = d.weighting;
        builtDistance_ = d.distanceCompensation;
        if (d.layout.empty()) {
            diag_.speakerOutputs.store(0, std::memory_order_relaxed);
        } else {
            speakers_.request(layoutFor(d), engineOrder_,
                              d.weighting == 1 ? bambi::Weighting::MaxRe : bambi::Weighting::Basic);
            if (speakers_.work()) {
                diag_.speakerOutputs.store(speakers_.design().outputs, std::memory_order_relaxed);
                virtual_.publishLayout(speakers_.design());
            }
        }
    }
}

void BambiDecoderProcessor::buildMatrix(const bambi::DecoderState& d) {
    const auto stop = [this] {
        //  the audio thread may be inside the convolver: wait for it to pass a block before the buffers move
        const bool wasPlaying = matrixPlays_.exchange(false, std::memory_order_acq_rel);
        if (!wasPlaying) return;
        const std::uint64_t before = blocksDone_.load(std::memory_order_acquire);
        for (int i = 0; i < 100 && blocksDone_.load(std::memory_order_acquire) == before; ++i) juce::Thread::sleep(1);
    };
    if (d.matrixFile.empty()) {
        stop();
        matrixNote_.clear();
        matrixError_.clear();
        return;
    }
    if (matrixDataFile_ != d.matrixFile) {
        std::string error;
        if (!bambi::readSofaMatrix(d.matrixFile, matrixData_, &error)) {
            stop();
            matrixDataFile_.clear();
            matrixError_ = juce::File(d.matrixFile).existsAsFile()
                               ? "the filter matrix could not be read: " + juce::String(error)
                               : "the filter matrix file is not there: " + juce::String(d.matrixFile);
            matrixNote_.clear();
            diag_.speakerOutputs.store(0, std::memory_order_relaxed);
            return;
        }
        matrixDataFile_ = d.matrixFile;
    }
    const bambi::FirMatrix& m = matrixData_;
    if (m.outputs > kMaxOutputs) {
        stop();
        matrixError_ = "the filter matrix has " + juce::String(m.outputs) + " outputs; " + juce::String(kMaxOutputs) +
                       " is the most a bus holds";
        diag_.speakerOutputs.store(0, std::memory_order_relaxed);
        return;
    }
    //  The filters at the host's rate, the field's channels as its inputs: a file with fewer inputs leaves the higher
    //  channels unheard, one with more drops its extra inputs. The input's normalisation as the file says, or as chosen.
    const int C = bambi::numChannels(engineOrder_);
    const int inputs = std::min(C, m.inputs);
    const bool n3d = d.matrixNormalization == 2 || (d.matrixNormalization == 0 && m.normalization != "SN3D");
    std::vector<float> one, scaled;
    bambi::resampleSinc(m.filter(0, 0), m.sampleRate, sampleRate_, one);
    const int taps = static_cast<int>(one.size());
    std::vector<float> filters(
        static_cast<std::size_t>(C) * static_cast<std::size_t>(m.outputs) * static_cast<std::size_t>(taps), 0.0f);
    for (int i = 0; i < inputs; ++i) {
        const float factor = n3d ? static_cast<float>(std::sqrt(2.0 * bambi::acnOrder(i) + 1.0)) : 1.0f;
        for (int o = 0; o < m.outputs; ++o) {
            bambi::resampleSinc(m.filter(i, o), m.sampleRate, sampleRate_, scaled);
            float* dst = filters.data() + (static_cast<std::size_t>(i) * static_cast<std::size_t>(m.outputs) +
                                           static_cast<std::size_t>(o)) *
                                              static_cast<std::size_t>(taps);
            for (std::size_t t = 0; t < scaled.size() && t < static_cast<std::size_t>(taps); ++t)
                dst[t] = scaled[t] * factor;
        }
    }
    stop();
    const bool ready =
        matrix_.prepareMatrix(C, m.outputs, taps, kPartition) && matrix_.publish(filters, C, m.outputs, taps);
    if (!ready) {
        matrixError_ = "the filter matrix could not be built";
        diag_.speakerOutputs.store(0, std::memory_order_relaxed);
        return;
    }
    matrix_.reset();
    matrixError_.clear();
    matrixNote_ =
        juce::String(m.inputs) + " in (order " +
        juce::String(m.order >= 0 ? m.order : bambi::host::orderThatFits(m.inputs)) + ", " + (n3d ? "n3d" : "sn3d") +
        "), " + juce::String(m.outputs) + " out, " + juce::String(m.taps) + " taps at " +
        juce::String(m.sampleRate / 1000.0, 1) + " khz" +
        (std::abs(m.sampleRate - sampleRate_) > 1.0 ? ", resampled to " + juce::String(sampleRate_ / 1000.0, 1) + " khz"
                                                    : juce::String()) +
        (inputs < C ? "; field channels past " + juce::String(inputs) + " unheard" : juce::String());
    diag_.speakerOutputs.store(m.outputs, std::memory_order_relaxed);
    matrixPlays_.store(true, std::memory_order_release);
}

bool BambiDecoderProcessor::loadMatrixFile(const juce::File& file, juce::String* error) {
    bambi::FirMatrix m;
    std::string why;
    if (!bambi::readSofaMatrix(file.getFullPathName().toStdString(), m, &why)) {
        if (error) *error = juce::String(why);
        return false;
    }
    if (m.outputs > kMaxOutputs) {
        if (error)
            *error = juce::String(m.outputs) + " outputs; " + juce::String(kMaxOutputs) + " is the most a bus holds";
        return false;
    }
    const std::string path = file.getFullPathName().toStdString();
    const std::string name = m.title.empty() ? file.getFileNameWithoutExtension().toStdString() : m.title;
    const int outputs = m.outputs;
    {
        const std::lock_guard<std::mutex> work(workMutex_);
        matrixData_ = std::move(m);
        matrixDataFile_ = path;
    }
    editDecoder("load filter matrix", [path, name, outputs](bambi::DecoderState& s) {
        s.matrixFile = path;
        s.matrixOutputs = outputs;
        s.layout.clear();
        s.layoutName = name;
    });
    return true;
}

juce::String BambiDecoderProcessor::matrixNote() const {
    const std::lock_guard<std::mutex> work(const_cast<std::mutex&>(workMutex_));
    return matrixNote_;
}

// ---- the tracker ---------------------------------------------------------------------------

void BambiDecoderProcessor::connectTracker(const bambi::DecoderState& d) {
    const bool airpods = d.trackerSource == 2, phone = d.trackerSource == 3;
    {
        const std::lock_guard<std::mutex> lock(trackerMutex_);
        trackerRead_.flip = d.flip;
        //  AirPods report their own frame, as the calibration found it or as Apple documents it; a tracker over OSC
        //  and the phone's page report the head's axes already.
        trackerRead_.mounting = airpods ? bambi::Mounting::Axes : bambi::Mounting::AsSent;
        for (std::size_t i = 0; i < 9; ++i) trackerRead_.axes[i] = d.airpodsAxes[i];
        trackerSource_ = d.trackerSource;
        if (!airpods && !phone) helperStatus_.clear();
        if (!phone) {
            phoneUrl_.clear();
            phoneStatus_.clear();
        }
    }
    //  Every source arrives as OSC on the port: a tracker from anywhere, the helper from this computer.
    const bool want = d.trackerSource == 1 || airpods || phone;
    if (want && (!oscConnected_ || oscPort_ != d.oscPort)) {
        if (oscConnected_) osc_.disconnect();
        //  Bound to this computer until asked: a port open on the network is open to the whole network.
        oscConnected_ = d.oscNetwork ? osc_.connect(d.oscPort) : osc_.connect(d.oscPort);
        oscPort_ = d.oscPort;
    } else if (!want && oscConnected_) {
        osc_.disconnect();
        oscConnected_ = false;
    }
    //  The helper runs while AirPods or a phone are the source, and is ended when neither is: it asks for the motion
    //  permission in its own name, serves the phone page in its own process, and quits with the host. A change
    //  between the two modes relaunches it.
    if ((airpods || phone) && oscConnected_) {
        if (helper_.running() && helperPhone_ != phone) {
            helper_.relaunch(HelperLink::bundledHelperPath(), d.oscPort, phone);
            helperPhone_ = phone;
        } else if (!helper_.running() && helper_.launch(HelperLink::bundledHelperPath(), d.oscPort, phone)) {
            helperPhone_ = phone;
        }
    } else {
        helper_.terminate();
    }
}

bool BambiDecoderProcessor::captureLevel() {
    const std::lock_guard<std::mutex> lock(trackerMutex_);
    if (!rawSeen_) return false;
    levelPose_ = rawNow_;
    levelCaptured_ = true;
    return true;
}

bool BambiDecoderProcessor::captureNod() {
    bambi::Mat3 nodded;
    {
        const std::lock_guard<std::mutex> lock(trackerMutex_);
        if (!rawSeen_ || !levelCaptured_) return false;
        nodded = rawNow_;
    }
    bambi::Mat3 axes;
    if (!bambi::axesFromLookAndNod(levelPose_, nodded, axes)) return false;
    levelCaptured_ = false;
    editDecoder("calibrate airpods", [axes](bambi::DecoderState& s) {
        for (std::size_t i = 0; i < 9; ++i) s.airpodsAxes[i] = axes[i];
        s.airpodsCalibrated = true;
    });
    recentre();
    return true;
}

void BambiDecoderProcessor::forgetCalibration() {
    levelCaptured_ = false;
    editDecoder("forget calibration", [](bambi::DecoderState& s) {
        s.airpodsAxes = bambi::DecoderState{}.airpodsAxes;
        s.airpodsCalibrated = false;
    });
}

juce::String BambiDecoderProcessor::phoneUrl() const {
    const std::lock_guard<std::mutex> lock(trackerMutex_);
    return phoneUrl_;
}

juce::String BambiDecoderProcessor::phoneStatus() const {
    const std::lock_guard<std::mutex> lock(trackerMutex_);
    return phoneStatus_;
}

juce::String BambiDecoderProcessor::helperStatus() const {
    const std::lock_guard<std::mutex> lock(trackerMutex_);
    return helperStatus_;
}

void BambiDecoderProcessor::oscMessageReceived(const juce::OSCMessage& message) {
    bambi::OrientationRead read;
    int source = 0;
    {
        const std::lock_guard<std::mutex> lock(trackerMutex_);
        read = trackerRead_;
        source = trackerSource_;
    }
    const juce::String address = message.getAddressPattern().toString();
    if (address == "/bambi/helper/status") {
        if (message.size() == 1 && message[0].isString()) {
            const std::lock_guard<std::mutex> lock(trackerMutex_);
            helperStatus_ = message[0].getString();
        }
        return;
    }
    if (address == "/bambi/helper/phone") {
        if (message.size() == 2 && message[0].isString() && message[1].isString()) {
            const std::lock_guard<std::mutex> lock(trackerMutex_);
            phoneUrl_ = message[0].getString();
            phoneStatus_ = message[1].getString();
        }
        return;
    }
    if (!bambi::knownForm(address.toStdString(), read.form)) return;  // learn mode reads the rest, later
    //  The helper's own sources arrive by their own address; another form on the port is not theirs to turn the head.
    if (source == 2 && address != "/quaternion") return;
    if (source == 3 && address != "/bambi/phone/ypr") return;
    std::array<float, 4> values{};
    std::size_t count = 0;
    for (const juce::OSCArgument& a : message) {
        if (count >= values.size()) break;
        if (a.isFloat32())
            values[count++] = a.getFloat32();
        else if (a.isInt32())
            values[count++] = static_cast<float>(a.getInt32());
    }
    bambi::Mat3 head;
    if (!bambi::headFromValues(read, std::span<const float>(values.data(), count), head)) return;
    tracker_.write(head);
    {
        //  the device's own orientation too, for the calibration: the same values read as sent
        bambi::OrientationRead raw = read;
        raw.mounting = bambi::Mounting::AsSent;
        bambi::Mat3 device;
        if (bambi::headFromValues(raw, std::span<const float>(values.data(), count), device)) {
            const std::lock_guard<std::mutex> lock(trackerMutex_);
            rawNow_ = device;
            rawSeen_ = true;
        }
    }
    diag_.trackerMessages.fetch_add(1, std::memory_order_relaxed);
    diag_.trackerLastMs.store(juce::Time::currentTimeMillis(), std::memory_order_relaxed);
}

// ---- state edits ---------------------------------------------------------------------------

void BambiDecoderProcessor::editDecoder(std::string_view name,
                                        const std::function<void(bambi::DecoderState&)>& change) {
    document().edit(name, [&change](bambi::PluginState& s) { change(s.decoder); });
}

bambi::DecoderState BambiDecoderProcessor::decoderState() const {
    return const_cast<BambiDecoderProcessor&>(*this).document().committed().decoder;
}

std::vector<int> BambiDecoderProcessor::channelsOf(Output which) const {
    const bambi::DecoderState d = decoderState();
    if (which == Output::Headphones) return {d.headphoneChannel, d.headphoneChannel + 1};
    return layoutChannels(d);
}

void BambiDecoderProcessor::playOutput(Output which, bool on) {
    const int bus = getChannelCountOfBus(false, 0);
    const std::vector<int> mine = channelsOf(which);
    const int needs = mine.empty() ? 0 : *std::max_element(mine.begin(), mine.end());
    notice_.clear();
    if (on && (mine.empty() || needs > bus)) {
        notice_ = mine.empty() ? juce::String("no layout")
                               : juce::String(needs) + " channels needed; the bus has " + juce::String(bus);
        return;
    }
    const Output other = which == Output::Headphones ? Output::Speakers : Output::Headphones;
    const std::vector<int> theirs = channelsOf(other);
    bool collide = false;
    for (int c : mine) collide = collide || std::find(theirs.begin(), theirs.end(), c) != theirs.end();
    const bambi::DecoderState d = decoderState();
    const bool otherOn = other == Output::Headphones ? d.headphonesPlay : d.speakersPlay;
    if (on && collide && otherOn)
        notice_ = juce::String(other == Output::Headphones ? "headphones" : "speakers") + " stopped: the same channels";
    editDecoder(on ? "play output" : "stop output", [which, on, collide, other](bambi::DecoderState& s) {
        (which == Output::Headphones ? s.headphonesPlay : s.speakersPlay) = on;
        if (on && collide) (other == Output::Headphones ? s.headphonesPlay : s.speakersPlay) = false;
    });
}

juce::String BambiDecoderProcessor::notice() const { return notice_; }

juce::String BambiDecoderProcessor::designError() const {
    const std::lock_guard<std::mutex> work(const_cast<std::mutex&>(workMutex_));
    return matrixError_.isNotEmpty() ? matrixError_ : juce::String(speakers_.lastError());
}

bambi::SpeakerDesign BambiDecoderProcessor::speakerDesign() const {
    const std::lock_guard<std::mutex> work(const_cast<std::mutex&>(workMutex_));
    return speakers_.design();
}

void BambiDecoderProcessor::describe(bambi::LinkStatic& st) const {
    //  The layout while it plays: every scene may draw it.
    const bambi::DecoderState d = decoderState();
    st.speakerCount = 0;
    if (!d.speakersPlay) return;
    for (const bambi::Loudspeaker& s : d.layout) {
        if (st.speakerCount >= static_cast<std::uint32_t>(bambi::kLinkMaxSpeakers)) break;
        bambi::LinkSpeaker& out = st.speakers[st.speakerCount++];
        out.azimuthDeg = static_cast<float>(s.azimuthDeg);
        out.elevationDeg = static_cast<float>(s.elevationDeg);
        out.channel = static_cast<std::uint8_t>(std::clamp(s.channel, 0, 255));
        out.imaginary = s.imaginary ? 1 : 0;
    }
}

// ---- audio ---------------------------------------------------------------------------------

void BambiDecoderProcessor::renderBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    bambi::ScopedFlushDenormals flushDenormals;
    const int n = buffer.getNumSamples();
    if (n <= 0) return;
    adoptPendingSnapshot();
    catchLearnedNote(midi);
    intakeParameters();

    auto in = getBusBuffer(buffer, true, 0);
    auto out = getBusBuffer(buffer, false, 0);
    const int inChannels = in.getNumChannels(), outChannels = out.getNumChannels();
    diag_.mainInputChannels.store(inChannels, std::memory_order_relaxed);
    diag_.outputChannels.store(outChannels, std::memory_order_relaxed);
    diag_.order.store(engineOrder_, std::memory_order_relaxed);
    diag_.nonRealtime.store(isNonRealtime(), std::memory_order_relaxed);
    if (auto* head = getPlayHead()) {
        const auto position = head->getPosition();
        diag_.playing.store(position.hasValue() && position->getIsPlaying(), std::memory_order_relaxed);
        diag_.transportKnown.store(position.hasValue(), std::memory_order_relaxed);
    }

    const bool ready = engineOrder_ >= 0 && modelLoaded_ && n <= maxBlock_ &&
                       bambi::host::orderThatFits(inChannels) == engineOrder_ && outChannels >= 2;
    if (!ready) {
        //  The layout changed without a prepareToPlay, or a host exceeds its own maximum. Silence rather than a
        //  guess, and never a resize on this thread.
        buffer.clear();
        diag_.layoutMismatches.fetch_add(1, std::memory_order_relaxed);
        diag_.outputPeak.store(0.0f, std::memory_order_relaxed);
        for (auto& p : diag_.channelPeak) p.store(0.0f, std::memory_order_relaxed);
        return;
    }

    //  The field, copied out before the outputs are written: on an in-place host they share the buffer.
    const int C = bambi::numChannels(engineOrder_);
    field_.read(in, 0, n);
    float inPeak = 0.0f;
    for (int c = 0; c < std::min(inChannels, C); ++c) inPeak = std::max(inPeak, in.getMagnitude(c, 0, n));
    diag_.inputPeak.store(inPeak, std::memory_order_relaxed);

    //  The head: the tracker while it overrides and has spoken, else yaw, pitch and roll as the host has them.
    //  This thread is the rotation stage's one writer, so the choice is made here.
    const bambi::DecoderState& d = current().decoder;
    if (const bambi::Mat3* fresh = tracker_.latest()) {
        trackerNow_ = *fresh;
        trackerSeen_ = true;
    }
    bambi::Mat3 head;
    const bool tracked = d.trackerSource != 0 && d.trackerOverrides && trackerSeen_;
    if (tracked) {
        if (recentre_.exchange(false, std::memory_order_relaxed)) yaw0_ = bambi::yawDegOf(trackerNow_);
        head = bambi::recentred(trackerNow_, yaw0_);
    } else {
        recentre_.store(false, std::memory_order_relaxed);
        const auto& p = parameterValues();
        head = bambi::headFromYpr(p[static_cast<std::size_t>(decoderParam("head.yaw"))],
                                  p[static_cast<std::size_t>(decoderParam("head.pitch"))],
                                  p[static_cast<std::size_t>(decoderParam("head.roll"))]);
    }
    turn_.setOrientation(head);
    virtual_.setOrientation(head);
    {
        double y, pt, r;
        bambi::yprOf(head, y, pt, r);
        diag_.headYaw.store(static_cast<float>(y), std::memory_order_relaxed);
        diag_.headPitch.store(static_cast<float>(pt), std::memory_order_relaxed);
        diag_.headRoll.store(static_cast<float>(r), std::memory_order_relaxed);
    }

    //  The outputs, each a set of channels on the bus; what nothing writes is silent.
    for (int c = 0; c < outChannels; ++c) out.clear(c, 0, n);
    //  The level glides per sample toward the parameter, a one-pole with a fixed coefficient, so the same block of
    //  audio comes out the same however the host cuts it.
    const float levelDb = parameterValues()[static_cast<std::size_t>(decoderParam("output.level"))];
    const float gain = levelDb <= -60.0f ? 0.0f : juce::Decibels::decibelsToGain(levelDb);
    for (int i = 0; i < n; ++i) {
        gainNow_ += (gain - gainNow_) * gainCoefficient_;
        gainRamp_[static_cast<std::size_t>(i)] = std::abs(gain - gainNow_) < 1e-7f ? (gainNow_ = gain) : gainNow_;
    }
    const auto ramp = [&](int i) { return gainRamp_[static_cast<std::size_t>(i)]; };
    const int speakerOutputs = diag_.speakerOutputs.load(std::memory_order_relaxed);
    const bool matrix = matrixPlays_.load(std::memory_order_acquire);

    bool hpHeard = false, spHeard = false;
    if (d.headphonesPlay && d.headphoneChannel >= 1 && d.headphoneChannel + 1 <= outChannels) {
        if (d.throughLayout && speakerOutputs > 0 && !matrix) {
            virtual_.process(field_.data(), C, stereo_.data(), n);
        } else {
            turn_.process(field_.data(), turned_.data(), n);
            binaural_.process(turned_.data(), stereo_.data(), n);
        }
        float* left = out.getWritePointer(d.headphoneChannel - 1);
        float* right = out.getWritePointer(d.headphoneChannel);
        for (int i = 0; i < n; ++i) {
            const float g = ramp(i);
            left[i] += stereo_[static_cast<std::size_t>(i) * 2] * g;
            right[i] += stereo_[static_cast<std::size_t>(i) * 2 + 1] * g;
        }
        hpHeard = true;
    }
    if (d.speakersPlay && speakerOutputs > 0 && speakerOutputs <= outChannels) {
        if (matrix)
            matrix_.process(field_.data(), speakersOut_.data(), n);
        else
            speakers_.process(field_.data(), C, speakersOut_.data(), speakerOutputs, n);
        for (int o = 0; o < speakerOutputs; ++o) {
            float* y = out.getWritePointer(o);
            for (int i = 0; i < n; ++i)
                y[i] += speakersOut_[static_cast<std::size_t>(i) * static_cast<std::size_t>(speakerOutputs) +
                                     static_cast<std::size_t>(o)] *
                        ramp(i);
        }
        spHeard = true;
    }
    diag_.headphonesHeard.store(hpHeard, std::memory_order_relaxed);
    diag_.speakersHeard.store(spHeard, std::memory_order_relaxed);
    blocksDone_.fetch_add(1, std::memory_order_release);
    float outPeak = 0.0f;
    for (int c = 0; c < outChannels; ++c) {
        const float peak = out.getMagnitude(c, 0, n);
        outPeak = std::max(outPeak, peak);
        if (c < static_cast<int>(diag_.channelPeak.size()))
            diag_.channelPeak[static_cast<std::size_t>(c)].store(peak, std::memory_order_relaxed);
    }
    diag_.outputPeak.store(outPeak, std::memory_order_relaxed);
}

juce::AudioProcessorEditor* BambiDecoderProcessor::createEditor() { return new BambiDecoderEditor(*this); }
