// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/decode/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <thread>

#include "bambi/decode/headmodelfile.hpp"
#include "bambi/decode/orientation.hpp"
#include "bambi/math/sh.hpp"

namespace bambi {

namespace {

std::size_t sz(int n) { return static_cast<std::size_t>(n); }

/// The layout as the design should see it: with the distances levelled when compensation is off.
Layout layoutFor(const DecoderState& d) {
    Layout l;
    l.speakers = d.layout;
    if (!d.distanceCompensation)
        for (Loudspeaker& s : l.speakers) s.radiusM = 1.0;
    return l;
}

/// Linear gain of a level in dB; -60 and below is silence.
float levelGain(float levelDb) { return levelDb <= -60.0f ? 0.0f : std::pow(10.0f, levelDb * 0.05f); }

/// One decimal place, with a point whatever the locale.
std::string decimal(double v) {
    const long tenths = std::lround(v * 10.0);
    return std::to_string(tenths / 10) + "." + std::to_string(std::labs(tenths % 10));
}

}  // namespace

void DecoderEngine::setHeads(const HeadModel& model, const HeadModel& ku100) {
    models_[0] = model;
    models_[1] = ku100;
    headsSet_ = true;
}

bool DecoderEngine::prepare(const DecoderState& d, int order, double sampleRate, int maxBlock, float levelDb) {
    if (!headsSet_ || order < 0) return false;
    order_ = order;
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    maxBlock_ = std::max(1, maxBlock);

    FilterDesign design;
    design.order = order_;
    design.sampleRate = sampleRate_;
    chosenHead_ = headIndex(d);
    binaural_.prepare(headModel(), design, kPartition);
    speakers_.prepare(order_, kMaxDecoderOutputs, sampleRate_, 0.1, kPartition);
    virtual_.prepare(headModel(), order_, sampleRate_, kPartition);
    sceneStage_.prepare(order_, sampleRate_);
    headStage_.prepare(order_, sampleRate_);
    clock_.reset();
    hopPos_ = 0;
    sceneNow_ = headNow_ = kIdentity3;
    headphonesHeard_ = speakersHeard_ = false;

    const std::size_t channels = sz(numChannels(order_));
    sceneField_.assign(sz(maxBlock_) * channels, 0.0f);
    turned_.assign(sz(maxBlock_) * channels, 0.0f);
    stereo_.assign(sz(maxBlock_) * 2, 0.0f);
    speakersOut_.assign(sz(maxBlock_) * sz(kMaxDecoderOutputs), 0.0f);
    gainRamp_.assign(sz(maxBlock_), 1.0f);
    gainCoefficient_ = 1.0f - std::exp(-1.0f / (0.01f * static_cast<float>(sampleRate_)));  // 10 ms
    gainNow_ = levelGain(levelDb);

    //  Everything built anew for this order and rate, before the first block.
    headBuilt_ = false;
    builtOrder_ = -1;
    builtMatrixOrder_ = -1;
    matrixPlays_.store(false, std::memory_order_relaxed);
    preparedInputs_ = preparedOutputs_ = preparedTaps_ = 0;
    speakerOutputs_.store(0, std::memory_order_relaxed);
    build(d);
    return true;
}

int DecoderEngine::headIndex(const DecoderState& d) {
    if (d.hrtfFileHead.text != fileHeadText_) {
        fileHeadText_ = d.hrtfFileHead.text;
        std::string error;
        fileLoaded_ = fileHeadText_ != nullptr && parseHeadModel(*fileHeadText_, fileModel_, &error);
    }
    if (d.hrtf == 2) return fileLoaded_ ? 2 : 0;  // a file head the session lost plays the model, not silence
    return d.hrtf == 1 ? 1 : 0;
}

void DecoderEngine::build(const DecoderState& d) {
    if (order_ < 0) return;
    //  Another head: the chain's synthesised head is replaced between builds, and the sliders are built on it anew.
    if (headIndex(d) != chosenHead_) {
        chosenHead_ = headIndex(d);
        binaural_.setModel(headModel());
        virtual_.setModel(headModel());
        headBuilt_ = false;
    }
    //  The listener's two sliders: the binaural filters, and the layout's array heard on headphones.
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
        builtMatrixOrder_ != order_) {
        const bool leaving = !builtMatrixFile_.empty() && d.matrixFile.empty();
        builtMatrixFile_ = d.matrixFile;
        builtMatrixNormalization_ = d.matrixNormalization;
        builtMatrixOrder_ = order_;
        buildMatrix(d);
        if (leaving) builtOrder_ = -1;  // the layout's design decides the outputs again
    }
    //  The layout's decoder, and the same layout heard on headphones. A pair's slice is narrow microphones at the
    //  loudspeakers, as narrow as the field's order allows, and follows the head.
    const Microphones microphones{d.pairSlice, d.pairSlice ? std::max(1, order_) : 1};
    if (d.matrixFile.empty() &&
        (builtOrder_ != order_ || builtLayout_ != d.layout || builtWeighting_ != d.weighting ||
         builtDistance_ != d.distanceCompensation || builtMicrophones_.atLoudspeakers != microphones.atLoudspeakers ||
         builtMicrophones_.pattern != microphones.pattern)) {
        builtOrder_ = order_;
        builtLayout_ = d.layout;
        builtWeighting_ = d.weighting;
        builtDistance_ = d.distanceCompensation;
        builtMicrophones_ = microphones;
        if (d.layout.empty()) {
            speakerOutputs_.store(0, std::memory_order_release);
        } else {
            speakers_.request(layoutFor(d), order_, d.weighting == 1 ? Weighting::MaxRe : Weighting::Basic,
                              microphones);
            if (speakers_.work()) {
                speakerOutputs_.store(speakers_.design().outputs, std::memory_order_release);
                virtual_.publishLayout(speakers_.design());
            }
        }
    }
    //  after the design, so a block never turns the field for a slice the array does not play yet
    pairFollows_.store(d.matrixFile.empty() && realLoudspeakers(d.layout) == 2 && d.pairSlice,
                       std::memory_order_release);
}

void DecoderEngine::holdMatrix(std::string path, FirMatrix matrix) {
    matrixData_ = std::move(matrix);
    matrixDataFile_ = std::move(path);
}

void DecoderEngine::stopMatrix() noexcept {
    //  Nothing new enters the convolver once the flag is down; a block already inside it is waited out, for at most
    //  200 ms, so the buffers do not move under the audio thread. Both flags are sequentially consistent with the
    //  audio thread's raise-then-check, so the two cannot miss each other.
    matrixPlays_.store(false, std::memory_order_seq_cst);
    for (int i = 0; i < 200 && inConvolver_.load(std::memory_order_seq_cst); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void DecoderEngine::buildMatrix(const DecoderState& d) {
    if (d.matrixFile.empty()) {
        stopMatrix();
        matrixNote_.clear();
        matrixError_.clear();
        return;
    }
    if (matrixDataFile_ != d.matrixFile) {
        std::string error;
        if (!readSofaMatrix(d.matrixFile, matrixData_, &error)) {
            stopMatrix();
            matrixDataFile_.clear();
            matrixError_ = "the filter matrix could not be read: " + error + ": " + d.matrixFile;
            matrixNote_.clear();
            speakerOutputs_.store(0, std::memory_order_release);
            return;
        }
        matrixDataFile_ = d.matrixFile;
    }
    const FirMatrix& m = matrixData_;
    const auto refuse = [this](std::string why) {
        stopMatrix();
        matrixError_ = std::move(why);
        speakerOutputs_.store(0, std::memory_order_release);
    };
    if (m.outputs > kMaxDecoderOutputs) {
        refuse("the filter matrix has " + std::to_string(m.outputs) + " outputs; " +
               std::to_string(kMaxDecoderOutputs) + " is the most a bus holds");
        return;
    }
    //  The filters at the engine's rate, the field's channels as its inputs: a file with fewer inputs leaves the
    //  higher channels unheard, one with more drops its extra inputs. The input's normalisation as the file says, or
    //  as chosen.
    const int C = numChannels(order_);
    const bool n3d = d.matrixNormalization == 2 || (d.matrixNormalization == 0 && m.normalization != "SN3D");
    std::vector<float> filters;
    int taps = 0;
    const int inputs = resampledFilters(m, C, sampleRate_, n3d, filters, taps);
    //  The convolver is reallocated only when its shape changes; a new set of the same shape -- another
    //  normalisation, the same file at another rate -- goes through the published triple buffer while it plays.
    const bool reshaped = C != preparedInputs_ || m.outputs != preparedOutputs_ || taps != preparedTaps_;
    if (reshaped) {
        stopMatrix();
        if (!matrix_.prepareMatrix(C, m.outputs, taps, kPartition)) {
            preparedInputs_ = preparedOutputs_ = preparedTaps_ = 0;
            refuse("the filter matrix could not be built");
            return;
        }
        preparedInputs_ = C;
        preparedOutputs_ = m.outputs;
        preparedTaps_ = taps;
        matrix_.reset();
    }
    if (!matrix_.publish(filters, C, m.outputs, taps)) {
        refuse("the filter matrix could not be built");
        return;
    }
    matrixError_.clear();
    matrixNote_ =
        std::to_string(m.inputs) + " in (order " + std::to_string(m.order >= 0 ? m.order : orderFitting(m.inputs)) +
        ", " + (n3d ? "n3d" : "sn3d") + "), " + std::to_string(m.outputs) + " out, " + std::to_string(m.taps) +
        " taps at " + decimal(m.sampleRate / 1000.0) + " khz" +
        (std::abs(m.sampleRate - sampleRate_) > 1.0 ? ", resampled to " + decimal(sampleRate_ / 1000.0) + " khz"
                                                    : std::string()) +
        (inputs < C ? "; field channels past " + std::to_string(inputs) + " unheard" : std::string());
    speakerOutputs_.store(m.outputs, std::memory_order_release);
    matrixPlays_.store(true, std::memory_order_release);
}

std::string DecoderEngine::designError() const { return matrixError_.empty() ? speakers_.lastError() : matrixError_; }

void DecoderEngine::process(const DecoderState& d, const DecoderFrame& c, const float* field, float* const* out,
                            int outChannels, int frames) noexcept {
    const int n = std::min(frames, maxBlock_);
    if (n <= 0 || order_ < 0) return;
    const int C = numChannels(order_);

    //  The scene's turn: yaw, pitch and roll as set, their rates integrated while they run, the whole held at zero
    //  while static. The tracker is an offset on top: a head, which the binaural decode and the slice undo so the
    //  scene stays in the room, or one more turn of the scene.
    const auto sceneAfter = [&](double dt) {
        Mat3 scene = kIdentity3;
        if (!d.sceneStatic) {
            clock_.advance(c.yawRateDeg, c.pitchRateDeg, c.rollRateDeg, c.ratesRun ? dt : 0.0);
            scene = headFromYpr(c.yawDeg + clock_.yawRad() * kRad2Deg, c.pitchDeg + clock_.pitchRad() * kRad2Deg,
                                c.rollDeg + clock_.rollRad() * kRad2Deg);
        }
        if (d.trackerTurnsScene) scene = multiply(c.tracker, scene);
        return scene;
    };
    headNow_ = d.trackerTurnsScene ? kIdentity3 : c.tracker;
    //  Set at each rotation hop's first frame, for the whole hop, counted from prepare: the stage reads its target
    //  there, so the turn and the clock land on the same frames however the host cuts its blocks.
    for (int done = 0; done < n;) {
        const int into = static_cast<int>(hopPos_ % kRotationHop);
        const int len = std::min(n - done, kRotationHop - into);
        if (into == 0) {
            sceneNow_ = sceneAfter(static_cast<double>(kRotationHop) / sampleRate_);
            sceneStage_.setOrientation(transposed(sceneNow_));  // the stage turns the field against what it is given
        }
        sceneStage_.process(field + sz(done) * sz(C), sceneField_.data() + sz(done) * sz(C), len);
        done += len;
        hopPos_ += len;
    }
    headStage_.setOrientation(headNow_);
    virtual_.setOrientation(headNow_);

    for (int ch = 0; ch < outChannels; ++ch) std::fill_n(out[ch], n, 0.0f);
    //  The level glides per sample toward its target, a one-pole with a fixed coefficient, so the same block of
    //  audio comes out the same however the host cuts it.
    const float gain = levelGain(c.levelDb);
    for (int i = 0; i < n; ++i) {
        gainNow_ += (gain - gainNow_) * gainCoefficient_;
        gainRamp_[sz(i)] = std::abs(gain - gainNow_) < 1e-7f ? (gainNow_ = gain) : gainNow_;
    }
    //  The flag before the count: a matrix's count is stored before its flag is raised.
    const bool matrix = matrixPlays_.load(std::memory_order_seq_cst);
    const int speakerOutputs = speakerOutputs_.load(std::memory_order_acquire);
    //  The head undone, once a block and only for whoever needs it: the binaural decode, and a pair of loudspeakers
    //  whose slice follows the head.
    bool turned = false;
    const auto turnOnce = [&] {
        if (!turned) headStage_.process(sceneField_.data(), turned_.data(), n);
        turned = true;
    };

    headphonesHeard_ = speakersHeard_ = false;
    if (d.headphonesPlay && d.headphoneChannel >= 1 && d.headphoneChannel + 1 <= outChannels) {
        if (d.throughLayout && speakerOutputs > 0 && !matrix) {
            virtual_.process(sceneField_.data(), C, stereo_.data(), n);
        } else {
            turnOnce();
            binaural_.process(turned_.data(), stereo_.data(), n);
        }
        float* left = out[d.headphoneChannel - 1];
        float* right = out[d.headphoneChannel];
        for (int i = 0; i < n; ++i) {
            const float g = gainRamp_[sz(i)];
            left[i] += stereo_[sz(i) * 2] * g;
            right[i] += stereo_[sz(i) * 2 + 1] * g;
        }
        headphonesHeard_ = true;
    }
    const int firstOut = std::max(1, d.speakerFirstChannel) - 1;  // the layout's channel 1 counted from 0 on the bus
    if (d.speakersPlay && speakerOutputs > 0 && firstOut + speakerOutputs <= outChannels && !c.speakersHeld) {
        int feeds = speakerOutputs;
        if (matrix) {
            //  raised before the flag is read again: a rebuild that lowers the flag between the two waits for us, and
            //  one that ran between the first read and here is played with its own count, which the bus must hold
            inConvolver_.store(true, std::memory_order_seq_cst);
            feeds = 0;
            if (matrixPlays_.load(std::memory_order_seq_cst) && firstOut + matrix_.outputs() <= outChannels) {
                feeds = matrix_.outputs();
                matrix_.process(sceneField_.data(), speakersOut_.data(), n);
            }
            inConvolver_.store(false, std::memory_order_seq_cst);
        } else if (pairFollows_.load(std::memory_order_acquire)) {
            turnOnce();
            speakers_.process(turned_.data(), C, speakersOut_.data(), speakerOutputs, n);
        } else {
            speakers_.process(sceneField_.data(), C, speakersOut_.data(), speakerOutputs, n);
        }
        for (int o = 0; o < feeds; ++o) {
            float* y = out[firstOut + o];
            for (int i = 0; i < n; ++i) y[i] += speakersOut_[sz(i) * sz(feeds) + sz(o)] * gainRamp_[sz(i)];
        }
        speakersHeard_ = true;
    }
}

}  // namespace bambi
