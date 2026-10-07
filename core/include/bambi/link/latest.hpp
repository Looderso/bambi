// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace bambi {

/*  The newest value of T, handed from one real-time writer to one non-real-time reader: how the audio thread hands
 *  its state to the link bus without touching shared memory itself.
 *
 *  A triple buffer: writer and reader each own a slot outright, and trade through a third with one atomic exchange.
 *  write() is wait-free: a copy, then one exchange. read() is wait-free and never sees a torn value, since it never
 *  reads a slot the writer could be writing -- no retry loop, so no retry bound to get wrong. Values written
 *  between two reads are skipped, which is the point: the reader wants where the source is, not its history.
 *
 *  A T too big to copy -- a set of filters -- is handled in place: the writer fills staging() and publish()es,
 *  the reader takes latest() and keeps the pointer until its next latest(). The same three slots, no copy on
 *  either side; the reader's slot is its own until it asks again.
 */
template <typename T>
class LatestValue {
public:
    /// Writer thread only.
    void write(const T& value) noexcept {
        static_assert(std::is_trivially_copyable_v<T>,
                      "LatestValue copies T on the real-time thread; T must not allocate or lock");
        staging() = value;
        publish();
    }

    /// Reader thread only. True and overwrites `out` if written since the last read; otherwise `out` is untouched.
    bool read(T& out) noexcept {
        static_assert(std::is_trivially_copyable_v<T>,
                      "LatestValue copies T on the real-time thread; T must not allocate or lock");
        const T* v = latest();
        if (v == nullptr) return false;
        out = *v;
        return true;
    }

    /// Writer thread only: the slot the writer owns, to fill in place and then publish(). Valid until publish().
    T& staging() noexcept { return slots_[static_cast<std::size_t>(back_)]; }
    void publish() noexcept { back_ = middle_.exchange(back_ | kFresh, std::memory_order_acq_rel) & kIndex; }

    /// Reader thread only: whether something was published since the last latest(). Only the reader clears it.
    bool hasFresh() const noexcept { return (middle_.load(std::memory_order_acquire) & kFresh) != 0; }
    /// Reader thread only: the newest published value, in place, or nullptr when nothing was published since the
    /// last latest(). The pointer stays valid until the next latest() that returns non-null.
    const T* latest() noexcept {
        if (!hasFresh()) return nullptr;
        front_ = middle_.exchange(front_, std::memory_order_acq_rel) & kIndex;
        return &slots_[static_cast<std::size_t>(front_)];
    }

private:
    static_assert(std::atomic<int>::is_always_lock_free);

    static constexpr int kIndex = 0b011;
    static constexpr int kFresh = 0b100;

    //  At every instant back_, front_ and (middle_ & kIndex) are a permutation of {0, 1, 2}: the writer only
    //  touches slots_[back_], the reader only touches slots_[front_], and they are never equal.
    std::array<T, 3> slots_{};
    int back_{0};                 ///< writer's
    int front_{1};                ///< reader's
    std::atomic<int> middle_{2};  ///< in transit, plus the fresh flag
};

}  // namespace bambi
