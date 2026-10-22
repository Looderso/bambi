// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "bambi/decode/convolver.hpp"
#include "bambi/decode/decoder.hpp"
#include "bambi/decode/fill.hpp"
#include "bambi/decode/headmodel.hpp"
#include "bambi/decode/headrotation.hpp"
#include "bambi/decode/sofafile.hpp"
#include "bambi/decode/speakers.hpp"
#include "bambi/decode/virtualarray.hpp"
#include "bambi/link/latest.hpp"
#include "bambi/mod/modulation.hpp"
#include "bambi/patch/state.hpp"

/*  The Decoder's engine: a field in, the outputs it plays out, each a set of channels on one bus.
 *
 *      field --> scene's turn --+--> head undone --> binaural filters ------------+--> headphones' pair
 *                               |                                                 |
 *                               +--> the layout's array on headphones ------------+   (through the layout)
 *                               |
 *                               +--> layout's decoder: matrix, distance gains, delays --> loudspeakers
 *                               |      (with its gaps filled: the decoder after the dry window, held for the fill)
 *                               +--> the gaps: HRTF window, order-3 binaural, cancellation -+  added to them
 *                               +--> or a filter matrix's FIRs ---------------------------> its outputs
 *
 *  The scene's turn -- yaw, pitch and roll, their rates integrated -- is heard by every output; the
 *  head's, from a tracker, only by the headphones and by a pair of loudspeakers whose slice follows the
 *  head. Every output is scaled by the level, which glides per sample toward its target. While the layout's gaps
 *  are filled every output waits for the fill, so one latency stays true for all.
 *
 *  What the state asks for -- the head and its two sliders, the layout's design, a filter matrix -- is
 *  built by build(), on a worker, never the audio thread; prepare() builds it once before the first
 *  block, so a render is a function of the state alone and never of the worker's timing. The audio
 *  thread swaps finished sets in whole.
 */
namespace bambi {

/// What a block plays with besides the state: the host's parameters and the tracker's head, in their own units.
struct DecoderFrame {
    double yawDeg{0.0}, pitchDeg{0.0}, rollDeg{0.0};              ///< the scene's turn as set
    double yawRateDeg{0.0}, pitchRateDeg{0.0}, rollRateDeg{0.0};  ///< degrees a second
    bool ratesRun{true};       ///< the rates integrate: the transport plays, they continue, or there is no transport
    float levelDb{0.0f};       ///< the outputs' level; -60 and below is silence
    Mat3 tracker{kIdentity3};  ///< the tracker's orientation, recentred; the identity without one
    bool speakersHeld{false};  ///< the loudspeakers' feeds held back, as while one loudspeaker is tested alone
};

class DecoderEngine {
public:
    static constexpr int kPartition = 64;  ///< the convolutions' block, and the latency every output lands at
                                           ///< while no fill plays

    /// The two built-in heads a state picks between: the model, and the KU100 as measured. Before prepare().
    void setHeads(const HeadModel& model, const HeadModel& ku100);

    /*  Allocates everything for a field of `order` (0..7) at `sampleRate`, for blocks of at most `maxBlock` and a
        bus of at most kMaxDecoderOutputs channels, then builds what `d` asks for. The level starts at `levelDb`, so
        the first block does not fade in from unity. False without heads. Never while build() or process() may run. */
    bool prepare(const DecoderState& d, int order, double sampleRate, int maxBlock, float levelDb);

    /*  Worker, one at a time: designs what `d` asks for that is not built yet -- the head and its sliders, the
        layout's decoder and the array on headphones, a filter matrix read from its file -- and publishes each
        whole. A matrix being replaced by one of another shape is waited out of the audio thread first, for at
        most 200 ms. Allocates. */
    void build(const DecoderState& d);
    /// Worker: a filter matrix already read from `path`, so the build that follows need not read it again.
    void holdMatrix(std::string path, FirMatrix matrix);

    /// Audio thread: the rates' integrated turn back to zero, as at a start or a locate.
    void restartRates() noexcept { clock_.reset(); }

    /*  Audio thread. `field`: `frames` interleaved frames of numChannels(order); `out`: `outChannels` channels of
        `frames`, cleared and written; the channels no output reaches stay silent. `frames` at most maxBlock. The
        scene's turn is set per rotation hop, counted from prepare, so the output does not depend on how the
        frames are cut. */
    void process(const DecoderState& d, const DecoderFrame& c, const float* field, float* const* out, int outChannels,
                 int frames) noexcept;

    //  What the last block did, for the owner's readouts. Audio thread.
    const float* sceneField() const noexcept { return sceneField_.data(); }  ///< the field turned by the scene
    const Mat3& head() const noexcept { return headNow_; }                   ///< the head the decode undid
    const Mat3& scene() const noexcept { return sceneNow_; }  ///< the scene's turn in force at the block's end
    bool headphonesHeard() const noexcept { return headphonesHeard_; }
    bool speakersHeard() const noexcept { return speakersHeard_; }

    /// Samples every output lands late: kPartition, and the fill's own while it plays. Any thread.
    int latency() const noexcept { return kPartition + fillExtra_.load(std::memory_order_acquire); }
    /// The layout's gaps play filled. Any thread.
    bool fillPlays() const noexcept { return fillExtra_.load(std::memory_order_acquire) > 0; }

    //  What is built. Read on the worker, or while it is idle.
    /// The loudspeaker outputs playing: the layout's design, or the filter matrix's; 0 for neither. Any thread.
    int speakerOutputs() const noexcept { return speakerOutputs_.load(std::memory_order_acquire); }
    int builtHead() const noexcept { return chosenHead_; }  ///< 0 the model, 1 the KU100, 2 a file's
    const HeadModel& headModel() const noexcept {
        return chosenHead_ == 2 ? fileModel_ : models_[static_cast<std::size_t>(chosenHead_)];
    }
    double builtEarShift() const noexcept { return builtShift_; }
    double builtHeadSize() const noexcept { return builtSize_; }
    const SpeakerDesign& speakerDesign() const noexcept { return speakers_.design(); }
    std::string designError() const;                                        ///< why nothing plays on the loudspeakers
    const std::string& matrixNote() const noexcept { return matrixNote_; }  ///< the matrix in a line; empty without one
    /// The fill in a line: why it does not play, or how far it keeps the ears apart; empty while it is off.
    const std::string& fillNote() const noexcept { return fillNote_; }

private:
    /// Which head `d` asks for and can have; parses the state's file head when its text changed.
    int headIndex(const DecoderState& d);
    void buildMatrix(const DecoderState& d);
    void stopMatrix() noexcept;  ///< nothing new enters the convolver, and a block inside it is waited out
    /// The fill for `d`, after the layout's design: designed and published whole, or stopped with its reason.
    void buildFill(const DecoderState& d, bool designChanged, bool headChanged);
    bool stopFill() noexcept;  ///< as stopMatrix, for the fill's convolvers; false when the audio thread stays in

    std::array<HeadModel, 2> models_;
    bool headsSet_{false};
    HeadModel fileModel_;
    std::shared_ptr<const std::string> fileHeadText_;  ///< the text fileModel_ was parsed from
    bool fileLoaded_{false};
    int chosenHead_{0};

    int order_{-1}, maxBlock_{0};
    double sampleRate_{48000.0};

    BinauralDecoder binaural_;
    SpeakerDecoder speakers_;
    VirtualArray virtual_;
    //  A decoder as filters, a SOFA FIR-E file's: read on the worker, resampled to the rate, published whole; the
    //  audio thread convolves with it in place of the layout's matrix while matrixPlays_ says so.
    MatrixConvolver matrix_;
    FirMatrix matrixData_;        ///< the file as read, kept so a change of normalisation needs no second read
    std::string matrixDataFile_;  ///< which file matrixData_ holds
    std::atomic<bool> matrixPlays_{false};
    std::atomic<bool> inConvolver_{false};  ///< the audio thread is inside the matrix convolver; a rebuild waits on it
    int preparedInputs_{0}, preparedOutputs_{0}, preparedTaps_{0};  ///< the convolver's shape as last allocated
    std::string builtMatrixFile_;
    int builtMatrixNormalization_{-1}, builtMatrixOrder_{-1};
    std::string matrixNote_, matrixError_;

    //  The built state: what build() compares a new state against.
    double builtShift_{0.0}, builtSize_{1.0};
    bool headBuilt_{false};
    std::vector<Loudspeaker> builtLayout_;
    int builtWeighting_{-1}, builtOrder_{-1};
    bool builtDistance_{true};
    Microphones builtMicrophones_;
    std::atomic<int> speakerOutputs_{0};
    std::atomic<bool> pairFollows_{false};  ///< the layout is a pair that follows the head: the turned field feeds it

    //  The layout's gaps filled (fill.hpp): its own order-3 head, the HRTF window and which output each cancellation
    //  row plays on, swapped in whole; the cancellation convolver reshaped only once the audio thread is out of it.
    struct FillSet {
        std::vector<float> window;  ///< (order + 1)^2 x 16: a field channel's row over the binaural part's channels
        std::vector<int> outputs;   ///< per cancellation row: the layout's output
    };
    BinauralFilterBuilder fillHead_;
    MatrixConvolver fillBinaural_, fillCancel_;
    LatestValue<FillSet> fillSets_;
    const FillSet* fillLive_{nullptr};
    std::atomic<bool> fillPlays_{false}, inFill_{false};
    std::atomic<int> fillExtra_{0};  ///< samples the fill adds to every output while it plays
    bool fillBuilt_{false}, fillHeadBuilt_{false};
    FillSettings builtFill_;
    int preparedFillRows_{0}, preparedFillTaps_{0};
    std::string fillNote_;

    //  The audio thread's.
    HeadRotation sceneStage_;  ///< the scene's turn, heard by every output
    HeadRotation headStage_;   ///< the head undone, for the binaural decode and the slice
    RotationClock clock_;      ///< the rates integrated
    std::int64_t hopPos_{0};   ///< frames since prepare: where in its rotation hop the scene's turn is set
    Mat3 sceneNow_{kIdentity3}, headNow_{kIdentity3};
    bool headphonesHeard_{false}, speakersHeard_{false};
    std::vector<float> sceneField_, turned_, stereo_, speakersOut_;  ///< scratch, sized in prepare
    std::vector<float> fillField_, fillEars_, fillOut_;              ///< the fill's scratch
    std::vector<float> fillWeight_;  ///< per frame of the block: the fill's gain, as the renderer fades it
    std::vector<float> headLine_;    ///< the headphones' pair, delayed while the fill plays: a ring of 2 x headLength_
    int headLength_{1}, headWrite_{0};
    int headWait_{0}, headWaitFrom_{0};     ///< samples read back: now, and the wait a crossfade leaves
    int headFade_{-1}, headFadeLength_{1};  ///< frames into that crossfade, -1 when none runs; its length
    bool headphonesWere_{false};            ///< the headphones played the block before
    std::vector<float> gainRamp_;           ///< the level per sample of the block
    float gainNow_{1.0f}, gainCoefficient_{1.0f};
};

}  // namespace bambi
