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
#include "bambi/host/ClapPorts.h"
#include "bambi/math/denormals.hpp"
#include "bambi/math/sh.hpp"

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
    bambi::HeadModel model, ku100;
    modelLoaded_ = bambi::parseHeadModel(
                       std::string_view(BinaryData::head_json, static_cast<std::size_t>(BinaryData::head_jsonSize)),
                       model, &error) &&
                   bambi::parseHeadModel(
                       std::string_view(BinaryData::ku100_json, static_cast<std::size_t>(BinaryData::ku100_jsonSize)),
                       ku100, &error);
    jassert(modelLoaded_);
    if (modelLoaded_) engine_.setHeads(model, ku100);
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

// ---- lifecycle -----------------------------------------------------------------------------

void BambiDecoderProcessor::prepare(double sampleRate, int maximumExpectedSamplesPerBlock) {
    //  The committed state first and on its own: committed() takes the document's lock itself, and the work lock
    //  never nests inside it the other way round than in committing().
    const bambi::DecoderState state = document().committed().decoder;
    const std::lock_guard<std::mutex> work(workMutex_);
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    maxBlock_ = std::max(1, maximumExpectedSamplesPerBlock);
    engineOrder_ = bambi::host::orderThatFits(getChannelCountOfBus(true, 0));
    diag_.sampleRate.store(sampleRate_, std::memory_order_relaxed);
    modulation().prepare(sampleRate_);
    setLoadRate(sampleRate_);
    setLatencySamples(kPartition);
    transport_.forget();
    if (engineOrder_ < 0 || !modelLoaded_) return;

    //  Everything that allocates happens here and in build(), never on the audio thread; what the state asks for is
    //  built before the first block, so what plays is a function of the state alone.
    field_.prepare(maxBlock_, engineOrder_);
    prepareEnergy(engineOrder_, sampleRate_, true);
    //  the level as the parameter has it, so the first block does not fade in from unity
    const float levelDb = parameterValues()[static_cast<std::size_t>(bambi::DecoderParam::OutputGain)];
    engine_.prepare(state, engineOrder_, sampleRate_, maxBlock_, levelDb);
    diag_.speakerOutputs.store(engine_.speakerOutputs(), std::memory_order_release);
    trackerSeen_ = false;
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

bool BambiDecoderProcessor::loadSofaFile(const juce::File& file, juce::String* error) {
    bambi::MeasuredHead measured;
    std::string why;
    if (!bambi::readSofa(file.getFullPathName().toStdString(), measured, &why)) {
        if (error) *error = juce::String(why);
        return false;
    }
    const bambi::HeadFit fit;  // the default fit: order 20 on the 0.2 ERB axis
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
    engine_.build(d);
    diag_.speakerOutputs.store(engine_.speakerOutputs(), std::memory_order_release);
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
        engine_.holdMatrix(path, std::move(m));
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
    const std::lock_guard<std::mutex> work(workMutex_);
    return juce::String(engine_.matrixNote());
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
        if (learnedAddresses_ != d.oscAddresses || learnedForm_ != d.oscForm) {
            learnedAddresses_ = d.oscAddresses;
            learnedForm_ = d.oscForm;
            splitSeen_ = {false, false, false};
        }
        if (d.trackerSource != 1) seen_.clear();
        if (!airpods && !phone) helperStatus_.clear();
        if (!phone) {
            phoneUrl_.clear();
            phoneStatus_.clear();
        }
    }
    //  Every source arrives as OSC on the port: a tracker from anywhere, the helper from this computer.
    const bool want = d.trackerSource == 1 || airpods || phone;
    if (want && (!oscConnected_ || oscPort_ != d.oscPort || oscNetwork_ != d.oscNetwork)) {
        if (oscConnected_) osc_.disconnect();
        //  Bound to this computer alone until asked -- the helper and the phone's page send from here -- or, open to
        //  the network, on every interface for a tracker from anywhere.
        oscSocket_ = std::make_unique<juce::DatagramSocket>(false);
        oscConnected_ = oscSocket_->bindToPort(d.oscPort, d.oscNetwork ? juce::String() : juce::String("127.0.0.1")) &&
                        osc_.connectToSocket(*oscSocket_);
        oscPort_ = d.oscPort;
        oscNetwork_ = d.oscNetwork;
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

std::vector<BambiDecoderProcessor::SeenAddress> BambiDecoderProcessor::seenAddresses() const {
    const std::lock_guard<std::mutex> lock(trackerMutex_);
    const std::int64_t now = juce::Time::currentTimeMillis();
    std::vector<SeenAddress> out;
    for (const SeenAddress& s : seen_)
        if (now - s.lastMs < 3000) out.push_back(s);
    return out;
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
    std::array<float, 4> values{};
    std::size_t count = 0;
    for (const juce::OSCArgument& a : message) {
        if (count >= values.size()) break;
        if (a.isFloat32())
            values[count++] = a.getFloat32();
        else if (a.isInt32())
            values[count++] = static_cast<float>(a.getInt32());
    }
    takeOrientation(address, std::span<const float>(values.data(), count));
}

void BambiDecoderProcessor::takeOrientation(const juce::String& address, std::span<const float> values) {
    bambi::OrientationRead read;
    int source = 0, form = 0;
    std::array<std::string, 3> learned;
    std::array<float, 3> gathered{};
    bool gatheredAll = false;
    {
        const std::lock_guard<std::mutex> lock(trackerMutex_);
        read = trackerRead_;
        source = trackerSource_;
        learned = learnedAddresses_;
        form = learnedForm_;
        if (source == 1) {
            //  the list learn mode shows: this address, its count and its first numbers
            const std::int64_t now = juce::Time::currentTimeMillis();
            auto it =
                std::find_if(seen_.begin(), seen_.end(), [&](const SeenAddress& s) { return s.address == address; });
            if (it == seen_.end() && seen_.size() < 16)
                it = seen_.insert(seen_.end(), SeenAddress{address, 0, {}, now});
            if (it != seen_.end()) {
                it->arguments = static_cast<int>(values.size());
                for (std::size_t i = 0; i < it->values.size(); ++i)
                    it->values[i] = i < values.size() ? values[i] : 0.0f;
                it->lastMs = now;
            }
        }
        //  split addresses: this message's first number into its slot, the triple once every slot has spoken
        const bool split =
            source == 1 && form != 0 && !learned[0].empty() && !learned[1].empty() && !learned[2].empty();
        if (split) {
            int slot = -1;
            for (int i = 0; i < 3; ++i)
                if (address.toStdString() == learned[static_cast<std::size_t>(i)]) slot = i;
            if (slot < 0 || values.empty()) return;
            splitValues_[static_cast<std::size_t>(slot)] = values[0];
            splitSeen_[static_cast<std::size_t>(slot)] = true;
            if (!(splitSeen_[0] && splitSeen_[1] && splitSeen_[2])) return;
            gathered = splitValues_;
            gatheredAll = true;
        }
    }
    const bool learnedWhole = source == 1 && form != 0 && !learned[0].empty() && !gatheredAll && learned[1].empty();
    if (gatheredAll) {
        //  three numbers cannot be a quaternion: a quaternion form chosen for split addresses reads as degrees
        read.form = form == 1 || form == 2 ? bambi::OrientationForm::YprDegrees : bambi::formFromIndex(form);
        values = std::span<const float>(gathered.data(), 3);
    } else if (learnedWhole) {
        if (address.toStdString() != learned[0]) return;
        read.form = bambi::formFromIndex(form);
    } else {
        if (!bambi::knownForm(address.toStdString(), read.form)) return;
        //  The helper's own sources arrive by their own address; another form on the port is not theirs to turn the head.
        if (source == 2 && address != "/quaternion") return;
        if (source == 3 && address != "/bambi/phone/ypr") return;
    }
    bambi::Mat3 head;
    if (!bambi::headFromValues(read, values, head)) return;
    tracker_.write(head);
    {
        //  the device's own orientation too, for the calibration: the same values read as sent
        bambi::OrientationRead raw = read;
        raw.mounting = bambi::Mounting::AsSent;
        bambi::Mat3 device;
        if (bambi::headFromValues(raw, values, device)) {
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

bambi::DecoderState BambiDecoderProcessor::decoderState() const { return document().committed().decoder; }

void BambiDecoderProcessor::playOutput(Output which, bool on) {
    const bambi::DecoderState now = decoderState();
    bambi::Lit lit = bambi::lightOutput(now, which, on, getChannelCountOfBus(false, 0));
    notice_ = juce::String::fromUTF8(lit.notice.c_str());
    if (lit.state == now) return;
    const bambi::DecoderState next = std::move(lit.state);
    editDecoder(on ? "play output" : "stop output", [next](bambi::DecoderState& s) { s = next; });
}

bambi::DecoderReport BambiDecoderProcessor::remoteReport() const {
    const auto* level = hostParameter(bambi::decoderParam("output.gain"));
    auto report = bambi::reportOf(decoderState(), getChannelCountOfBus(false, 0),
                                  level->convertFrom0to1(level->getValue()), notice_.toStdString());
    report.placeSerial = placeSerial_.load();
    report.placeOut = placeOut_.load();
    report.placeAt = placeAt_.load();
    return report;
}

void BambiDecoderProcessor::remoteRequest(const bambi::DecoderRequest& request) {
    if (request.host) host_ = request.host;
    const bambi::DecoderState now = decoderState();
    bambi::Lit lit = bambi::grant(now, request, getChannelCountOfBus(false, 0));
    notice_ = juce::String::fromUTF8(lit.notice.c_str());
    if (lit.state != now) {
        const bambi::DecoderState next = std::move(lit.state);
        editDecoder("outputs", [next](bambi::DecoderState& s) { s = next; });
    }
    if (request.levelDb) {
        auto* level = hostParameter(bambi::decoderParam("output.gain"));
        level->beginChangeGesture();
        level->setValueNotifyingHost(level->convertTo0to1(static_cast<float>(*request.levelDb)));
        level->endChangeGesture();
    }
}

juce::String BambiDecoderProcessor::notice() const { return notice_; }

juce::String BambiDecoderProcessor::designError() const {
    const std::lock_guard<std::mutex> work(workMutex_);
    return juce::String(engine_.designError());
}

bambi::SpeakerDesign BambiDecoderProcessor::speakerDesign() const {
    const std::lock_guard<std::mutex> work(workMutex_);
    return engine_.speakerDesign();
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
        out.channel = static_cast<std::uint8_t>(std::clamp(s.channel + d.speakerFirstChannel - 1, 0, 255));
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

    //  One input bus and one output bus, both from channel 0 of the host's buffer: indexed directly, since a bus
    //  view of 32 channels or more would allocate on this thread.
    const int inChannels = std::min(getChannelCountOfBus(true, 0), buffer.getNumChannels());
    const int outChannels = std::min(getChannelCountOfBus(false, 0), buffer.getNumChannels());
    diag_.mainInputChannels.store(inChannels, std::memory_order_relaxed);
    diag_.outputChannels.store(outChannels, std::memory_order_relaxed);
    diag_.order.store(engineOrder_, std::memory_order_relaxed);
    diag_.nonRealtime.store(isNonRealtime(), std::memory_order_relaxed);
    //  Where the host is, and whether it started or jumped: the shared reading, so the decoder and the effects agree
    //  on where a bounce's timeline restarted.
    const auto tp = transport_.observe(getPlayHead(), n, static_cast<std::int64_t>(n) + kTimelineSlack, sampleRate_);
    diag_.playing.store(tp.playing, std::memory_order_relaxed);
    diag_.transportKnown.store(tp.timeline >= 0, std::memory_order_relaxed);

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
    field_.read(bambi::host::busChannels(*this, buffer, true, 0), 0, n);
    float inPeak = 0.0f;
    for (int c = 0; c < std::min(inChannels, C); ++c) inPeak = std::max(inPeak, buffer.getMagnitude(c, 0, n));
    diag_.inputPeak.store(inPeak, std::memory_order_relaxed);

    //  What the engine plays with besides the state: the host's parameters, the tracker's head, the transport.
    const bambi::DecoderState& d = current().decoder;
    //  A bounce holds the pose the tracker had when it began: what arrives mid-render is a live value no file
    //  should bake in.
    if (!isNonRealtime()) {
        if (const bambi::Mat3* fresh = tracker_.latest()) {
            trackerNow_ = *fresh;
            trackerSeen_ = true;
        }
    }
    bambi::DecoderFrame controls;
    if (d.trackerSource != 0 && trackerSeen_) {
        if (recentre_.exchange(false, std::memory_order_relaxed)) yaw0_ = bambi::yawDegOf(trackerNow_);
        controls.tracker = bambi::recentred(trackerNow_, yaw0_);
    } else {
        recentre_.store(false, std::memory_order_relaxed);
    }
    //  The rates run while the transport plays, or always when set to continue; a start or a locate takes the
    //  integrated turn back to zero unless set to continue, and a host that says nothing of its transport runs them.
    const auto& p = parameterValues();
    const auto at = [&](bambi::ParamId id) { return static_cast<double>(p[static_cast<std::size_t>(id)]); };
    const bool continues = sharedValue(ratesRetriggerAt(), 0.0f) > 0.5f;
    if (tp.restart && !continues) engine_.restartRates();
    controls.ratesRun = tp.playing || continues || tp.timeline < 0;
    controls.yawDeg = at(bambi::DecoderParam::HeadYaw);
    controls.pitchDeg = at(bambi::DecoderParam::HeadPitch);
    controls.rollDeg = at(bambi::DecoderParam::HeadRoll);
    controls.yawRateDeg = at(bambi::DecoderParam::HeadYawRate);
    controls.pitchRateDeg = at(bambi::DecoderParam::HeadPitchRate);
    controls.rollRateDeg = at(bambi::DecoderParam::HeadRollRate);
    controls.levelDb = p[static_cast<std::size_t>(bambi::DecoderParam::OutputGain)];
    //  The test burst holds the array's feeds back, so the loudspeaker is heard alone.
    const int test = testChannel_.load(std::memory_order_acquire);
    controls.speakersHeld = test != 0;

    engine_.process(d, controls, field_.data(), buffer.getArrayOfWritePointers(), outChannels, n);

    //  The energy picture, while a window asks for it: the field as every output hears it, turned by the scene,
    //  is what arrived; nothing is added, so the picture is all of what came in.
    if (gatheringEnergy()) {
        gatherArrived(engine_.sceneField(), n);
        gatherNothingLeaving(n);
        publishEnergy();
    }
    {
        double y, pt, r;
        const bambi::Mat3& head = engine_.head();
        bambi::yprOf(head, y, pt, r);
        diag_.headYaw.store(static_cast<float>(y), std::memory_order_relaxed);
        diag_.headPitch.store(static_cast<float>(pt), std::memory_order_relaxed);
        diag_.headRoll.store(static_cast<float>(r), std::memory_order_relaxed);
        //  where the head looks and what plays, for every other instance's scene: a wait-free copy that linkTick publishes
        bambi::LinkDynamic published;
        const bambi::Vec3 gaze = bambi::apply(head, bambi::Vec3{1, 0, 0}),
                          left = bambi::apply(head, bambi::Vec3{0, 1, 0});
        published.headGazeX = static_cast<float>(gaze.x);
        published.headGazeY = static_cast<float>(gaze.y);
        published.headGazeZ = static_cast<float>(gaze.z);
        published.headLeftX = static_cast<float>(left.x);
        published.headLeftY = static_cast<float>(left.y);
        published.headLeftZ = static_cast<float>(left.z);
        published.playFlags = static_cast<std::uint8_t>((d.speakersPlay ? bambi::kLinkPlaySpeakers : 0) |
                                                        (d.headphonesPlay ? bambi::kLinkPlayHeadphones : 0) |
                                                        (d.throughLayout ? bambi::kLinkPlayThroughLayout : 0));
        published.sampledUs = bambi::linkNowMicros();
        link().publish(published);
        bambi::yprOf(engine_.scene(), y, pt, r);
        diag_.sceneYaw.store(static_cast<float>(y), std::memory_order_relaxed);
        diag_.scenePitch.store(static_cast<float>(pt), std::memory_order_relaxed);
        diag_.sceneRoll.store(static_cast<float>(r), std::memory_order_relaxed);
    }

    //  The test burst: noise on one channel, on top of whatever else plays there. Deterministic: the same xorshift
    //  sequence from the same seed at every start.
    const int testLength = static_cast<int>(0.4 * sampleRate_), testFade = static_cast<int>(0.01 * sampleRate_);
    if (test >= 1 && test <= outChannels) {
        int pos = testPos_.load(std::memory_order_relaxed);
        if (pos == 0) testNoise_ = 0x9e3779b9u;
        float* y = buffer.getWritePointer(test - 1);
        for (int i = 0; i < n && pos < testLength; ++i, ++pos) {
            testNoise_ ^= testNoise_ << 13;
            testNoise_ ^= testNoise_ >> 17;
            testNoise_ ^= testNoise_ << 5;
            const float white = static_cast<float>(testNoise_) / 2147483648.0f - 1.0f;  // -1..1
            const float edge =
                static_cast<float>(std::min({pos, testLength - 1 - pos, testFade})) / static_cast<float>(testFade);
            y[i] += white * 0.1f * std::min(edge, 1.0f);
        }
        testPos_.store(pos, std::memory_order_relaxed);
        if (pos >= testLength) {
            int played = test;  // a channel asked for during the block stays asked for
            testChannel_.compare_exchange_strong(played, 0, std::memory_order_acq_rel);
        }
    }
    diag_.headphonesHeard.store(engine_.headphonesHeard(), std::memory_order_relaxed);
    diag_.speakersHeard.store(engine_.speakersHeard(), std::memory_order_relaxed);
    float outPeak = 0.0f;
    for (int c = 0; c < outChannels; ++c) {
        const float peak = buffer.getMagnitude(c, 0, n);
        outPeak = std::max(outPeak, peak);
        if (c < static_cast<int>(diag_.channelPeak.size()))
            diag_.channelPeak[static_cast<std::size_t>(c)].store(peak, std::memory_order_relaxed);
    }
    diag_.outputPeak.store(outPeak, std::memory_order_relaxed);
}

juce::AudioProcessorEditor* BambiDecoderProcessor::createEditor() { return new BambiDecoderEditor(*this); }
