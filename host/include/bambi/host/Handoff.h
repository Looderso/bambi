// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

#include "bambi/link/spsc.hpp"

/*  State reaching the audio thread without a lock.
 *
 *  Every plugin in the suite does this identically and only the payload differs: the encoder hands
 *  over a compiled patch, a trajectory and a region; Echo hands over a patch and its send. So the
 *  payload is the template parameter and the protocol is here, once.
 *
 *  The protocol: the message thread builds a snapshot whole -- every allocation on its side -- and
 *  posts it. The audio thread takes it by pointer at a block boundary, pushes the one it was using
 *  onto a queue, and never frees anything. The message thread drains that queue.
 */
namespace bambi::host {

/// Counts what is alive and what was freed on the audio thread. The second number must stay zero: a test can only make that assertion if something counts.
struct SnapshotCensus {
    SnapshotCensus() noexcept;
    ~SnapshotCensus();

    SnapshotCensus(const SnapshotCensus&) = delete;
    SnapshotCensus& operator=(const SnapshotCensus&) = delete;

    static int live() noexcept;
    static int freedOnAudioThread() noexcept;

    /// processBlock declares itself, so a free inside one is observed rather than argued about.
    struct InAudioBlock {
        InAudioBlock() noexcept;
        ~InAudioBlock();
    };
};

/// `Snapshot` is the plugin's own payload, and inherits SnapshotCensus so what allocates is what is counted. Never modified once posted.
template <class Snapshot>
class Handoff {
public:
    ~Handoff() {
        delete pending_.exchange(nullptr, std::memory_order_acq_rel);
        freeRetired();
        delete current_;
    }

    /// The first snapshot, before any audio. Not a post: there is nothing yet to displace.
    void begin(std::unique_ptr<Snapshot> first) noexcept {
        delete current_;
        current_ = first.release();
    }

    /*  Message thread. Returns whatever was pending and never adopted, for the caller to drop --
     *  outside whatever lock it holds, since a delete is not something to do under one. The audio
     *  thread only ever takes `pending_` by exchanging it out, so a pointer still there is ours.
     *
     *  Drains the retire queue first, so that posting is all a caller has to do: the queue is
     *  sixteen deep, and a plugin that never drained it would fill it, at which point `adopt()`
     *  defers forever and no state reaches the audio thread again -- silently, with only
     *  `deferrals()` to show it. */
    [[nodiscard]] std::unique_ptr<Snapshot> post(std::unique_ptr<Snapshot> next) {
        freeRetired();
        return std::unique_ptr<Snapshot>(pending_.exchange(next.release(), std::memory_order_acq_rel));
    }

    /*  Audio thread. The newly adopted snapshot, or nullptr when there was nothing to adopt or no
     *  room to retire the current one. Real-time safe: bounded work, no allocation, no lock.
     *
     *  Room is checked before the pending snapshot is taken. With the queue full -- a message thread
     *  stalled through sixteen commits -- the block keeps what it has and the next one tries again.
     *  Nothing is freed here, and nothing is dropped. */
    Snapshot* adopt() noexcept {
        if (pending_.load(std::memory_order_relaxed) == nullptr) return nullptr;
        if (retired_.full()) {
            deferred_.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        Snapshot* next = pending_.exchange(nullptr, std::memory_order_acq_rel);
        if (next == nullptr) return nullptr;
        retired_.push(current_);
        current_ = next;
        return current_;
    }

    /*  What the audio thread has replaced since the last call. `post()` does this itself; call it
     *  directly only to free sooner than the next post -- from a timer, say.
     *
     *  Not the audio thread: it deletes. The queue has one producer, the audio thread, and one
     *  consumer at a time, which the mutex guarantees -- a host may load state from a thread of its
     *  own while an editor edits, so "the message thread" is more than one thread and two of them
     *  draining at once would be two consumers. The audio thread never takes it. */
    void freeRetired() {
        const std::lock_guard<std::mutex> lock(retireMutex_);
        Snapshot* old = nullptr;
        while (retired_.pop(old)) delete old;
    }

    /// The snapshot the audio thread is using. Const, because a posted snapshot is never modified -- the whole protocol rests on that. Null only before `begin()`, which every plugin calls in its constructor.
    const Snapshot* current() const noexcept { return current_; }

    /// Blocks that found the retire queue full. Nonzero is a stalled message thread, not a fault.
    std::uint64_t deferrals() const noexcept { return deferred_.load(std::memory_order_relaxed); }

private:
    Snapshot* current_{nullptr};               ///< the audio thread's
    std::atomic<Snapshot*> pending_{nullptr};  ///< message thread -> audio thread
    bambi::SpscQueue<Snapshot*, 16> retired_;  ///< audio thread -> message thread
    std::mutex retireMutex_;                   ///< consumer side only; never the audio thread
    std::atomic<std::uint64_t> deferred_{0};
};

}  // namespace bambi::host
