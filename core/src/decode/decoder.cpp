// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/decode/decoder.hpp"

#include <vector>

namespace bambi {

bool BinauralDecoder::prepare(const HeadModel& model, const FilterDesign& design, int partition, int rings,
                              int azimuths) {
    std::vector<Vec3> directions;
    std::vector<double> weights;
    quadratureGrid(rings, azimuths, directions, weights);
    if (!builder_.prepare(model, design, directions, weights)) return false;
    if (!convolver_.prepare(design.order, design.taps, partition)) return false;
    built_.store(requested_.load(std::memory_order_acquire) - 1,
                 std::memory_order_release);  // whatever was asked is unbuilt
    return true;
}

void BinauralDecoder::request(double earShiftErb, double headSize) noexcept {
    shift_.store(earShiftErb, std::memory_order_relaxed);
    size_.store(headSize, std::memory_order_relaxed);
    requested_.fetch_add(1, std::memory_order_release);
}

bool BinauralDecoder::work() noexcept {
    // The count is read before the sliders: a request that lands in between leaves the count ahead of what was
    // built, so the next work() builds again. Over-building is harmless; under-building would lose a slider move.
    const int seen = requested_.load(std::memory_order_acquire);
    if (seen == built_.load(std::memory_order_relaxed)) return false;
    builder_.build(shift_.load(std::memory_order_relaxed), size_.load(std::memory_order_relaxed));
    convolver_.publish(builder_);
    built_.store(seen, std::memory_order_release);
    return true;
}

}  // namespace bambi
