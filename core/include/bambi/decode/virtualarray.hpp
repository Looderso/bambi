// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vector>

#include "bambi/decode/binaural.hpp"
#include "bambi/decode/convolver.hpp"
#include "bambi/decode/headrotation.hpp"
#include "bambi/decode/speakers.hpp"

/*  The layout's loudspeakers heard on headphones: what that array will do to the mix -- its gaps, its
 *  order limit, its missing lower half -- heard at home from the room's own layout file. Each
 *  loudspeaker is a point source: its signal, as the array's decoder would play it, is encoded at a
 *  high order at the loudspeaker's direction, and the field of all of them is decoded binaurally at
 *  that order, well above the array's, so what is heard is the array's panning and not a second blur
 *  on top. Every loudspeaker sits at one distance: the room's distance gains and delays undo the
 *  room's own distances, which a virtual room does not have.
 *
 *  The two steps collapse into one matrix from the bus to the high-order field, which a
 *  SpeakerRenderer plays and swaps as it would a layout; the head's turn against the room goes on
 *  that field, and a BinauralConvolver takes it from there.
 */
namespace bambi {

inline constexpr int kVirtualArrayOrder = 7;

/// The array as an order-`fieldOrder` field: one row per field channel, the loudspeakers' rows encoded at their
/// directions and summed. No distance gains, no delays, no channel map: the rows are the field's channels.
SpeakerDesign fieldDesign(const SpeakerDesign& array, int fieldOrder = kVirtualArrayOrder);

class VirtualArray {
public:
    /// Allocates: a renderer from the bus to the field, the binaural filters at the field's order and their convolver.
    bool prepare(const HeadModel& model, int busOrder, double sampleRate, int partition = 64,
                 int fieldOrder = kVirtualArrayOrder);

    /// Worker: the array to hear, as designed for the room. Allocates (the field design); publishes it whole.
    bool publishLayout(const SpeakerDesign& array);
    /// Worker, between builds: another head; buildHead() then publishes its filters. Allocates the head's tables.
    void setModel(const HeadModel& model) { builder_.setModel(model); }
    /// Worker: the listener's two sliders. Allocates nothing; publishes the filters whole.
    void buildHead(double earShiftErb, double headSize) noexcept;
    /// Any thread: the head's orientation in the room; the array stays where it is.
    void setOrientation(const Mat3& headInWorld) noexcept { turn_.setOrientation(headInWorld); }

    /// Audio thread: `in` frames x inChannels of the bus, `out` frames of interleaved stereo; may not alias.
    void process(const float* in, int inChannels, float* out, int frames) noexcept;
    void reset() noexcept;

    int latency() const { return convolver_.latency(); }
    int fieldOrder() const { return fieldOrder_; }
    const BinauralFilterBuilder& filters() const { return builder_; }

private:
    static constexpr int kChunk = 64;
    int fieldOrder_{0}, fieldChannels_{0};
    SpeakerRenderer renderer_;
    HeadRotation turn_;
    BinauralFilterBuilder builder_;
    BinauralConvolver convolver_;
    std::vector<float> field_, turned_;  // kChunk x fieldChannels each
};

}  // namespace bambi
