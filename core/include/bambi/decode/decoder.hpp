// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>

#include "bambi/decode/binaural.hpp"
#include "bambi/decode/convolver.hpp"

/*  The decoder's binaural path across its three threads. The sliders arrive from anywhere through
 *  request(), which never blocks; one worker calls work(), which builds and publishes the filters for
 *  the newest sliders whenever they changed -- a change that lands during a build is built next time,
 *  never lost and never a second build at once; the audio thread calls process(). Nothing is heard
 *  until the first work(): the owner calls it before the first block, so what plays, and what a
 *  bounce renders, is a function of the parameters and never of the worker's timing. That holds
 *  only while the sliders stay put through a render: they are the listener's settings, not something
 *  a host automates -- a move mid-render would land on whichever block the worker reached first.
 */
namespace bambi {

class BinauralDecoder {
public:
    /// Allocates everything: the head, the design's grid (rings x azimuths), the builder and the convolver. Never
    /// while work() or process() may run.
    bool prepare(const HeadModel& model, const FilterDesign& design, int partition = 64, int rings = 12,
                 int azimuths = 20);

    /// Worker, between builds: another head, built at the next work(). Allocates the head's tables.
    void setModel(const HeadModel& model) {
        builder_.setModel(model);
        requested_.fetch_add(1, std::memory_order_release);
    }
    /// Any thread: the sliders as the listener set them. Marks the filters stale. The two land separately, so a
    /// build that starts between them pairs a new value with an old one; the count says so and the next work()
    /// builds the pair.
    void request(double earShiftErb, double headSize) noexcept;
    /// A request not yet built.
    bool stale() const noexcept {
        return requested_.load(std::memory_order_acquire) != built_.load(std::memory_order_acquire);
    }
    /// Worker thread, one at a time: builds and publishes the filters for the newest request when there is one.
    /// True when it built. Allocates nothing.
    bool work() noexcept;

    /// Audio thread: see MatrixConvolver::process.
    void process(const float* in, float* out, int frames) noexcept { convolver_.process(in, out, frames); }
    void reset() noexcept { convolver_.reset(); }

    int latency() const { return convolver_.latency(); }
    int channels() const { return convolver_.channels(); }
    /// The filters as last built, for a readout; read it on the worker, or when the worker is idle.
    const BinauralFilterBuilder& filters() const { return builder_; }

private:
    BinauralFilterBuilder builder_;
    MatrixConvolver convolver_;
    std::atomic<double> shift_{0.0}, size_{1.0};
    std::atomic<int> requested_{1}, built_{0};  // a fresh decoder has one request pending: its defaults
};

}  // namespace bambi
