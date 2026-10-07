// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/decode/virtualarray.hpp"

#include <algorithm>

#include "bambi/math/sh.hpp"

namespace bambi {

namespace {
std::size_t sz(int n) { return static_cast<std::size_t>(n); }
}  // namespace

SpeakerDesign fieldDesign(const SpeakerDesign& array, int fieldOrder) {
    SpeakerDesign field;
    field.order = array.order;
    field.outputs = numChannels(fieldOrder);
    const int C = numChannels(array.order), F = field.outputs;
    field.matrix.assign(sz(F) * sz(C), 0.0);
    std::vector<double> y(sz(F));
    // Each real loudspeaker's row, encoded at its direction: field[f][c] += Y_f(direction) * array[output][c].
    std::size_t real = 0;
    for (std::size_t p = 0; p < array.points.size(); ++p) {
        if (array.imaginary[p]) continue;
        const int output = array.channel[real++];
        shSN3D(array.points[p], fieldOrder, y);
        for (int f = 0; f < F; ++f)
            for (int c = 0; c < C; ++c)
                field.matrix[sz(f) * sz(C) + sz(c)] += y[sz(f)] * array.matrix[sz(output) * sz(C) + sz(c)];
    }
    field.points = array.points;
    field.imaginary = array.imaginary;
    field.ring = array.ring;
    return field;
}

bool VirtualArray::prepare(const HeadModel& model, int busOrder, double sampleRate, int partition, int fieldOrder) {
    fieldOrder_ = fieldOrder;
    fieldChannels_ = numChannels(fieldOrder);
    if (!renderer_.prepare(busOrder, fieldChannels_, sampleRate, 0.0)) return false;
    if (!turn_.prepare(fieldOrder, sampleRate)) return false;
    std::vector<Vec3> directions;
    std::vector<double> weights;
    quadratureGrid(12, 20, directions, weights);
    FilterDesign design;
    design.order = fieldOrder;
    design.sampleRate = sampleRate;
    if (!builder_.prepare(model, design, directions, weights)) return false;
    if (!convolver_.prepare(fieldOrder, design.taps, partition)) return false;
    field_.assign(sz(kChunk) * sz(fieldChannels_), 0.0f);
    turned_.assign(sz(kChunk) * sz(fieldChannels_), 0.0f);
    return true;
}

bool VirtualArray::publishLayout(const SpeakerDesign& array) {
    return renderer_.publish(fieldDesign(array, fieldOrder_));
}

void VirtualArray::buildHead(double earShiftErb, double headSize) noexcept {
    builder_.build(earShiftErb, headSize);
    convolver_.publish(builder_);
}

void VirtualArray::process(const float* in, int inChannels, float* out, int frames) noexcept {
    for (int done = 0; done < frames;) {
        const int n = std::min(kChunk, frames - done);
        renderer_.process(in + sz(done) * sz(inChannels), inChannels, field_.data(), fieldChannels_, n);
        turn_.process(field_.data(), turned_.data(), n);
        convolver_.process(turned_.data(), out + sz(done) * 2, n);
        done += n;
    }
}

void VirtualArray::reset() noexcept {
    renderer_.reset();
    turn_.reset();
    convolver_.reset();
}

}  // namespace bambi
