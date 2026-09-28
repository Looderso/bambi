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
 */
template <typename T>
class LatestValue {
public:
    /// Writer thread only.
    void write(const T& value) noexcept {
        slots_[static_cast<std::size_t>(back_)] = value;
        back_ = middle_.exchange(back_ | kFresh, std::memory_order_acq_rel) & kIndex;
    }

    /// Reader thread only. True and overwrites `out` if written since the last read; otherwise `out` is untouched.
    bool read(T& out) noexcept {
        if ((middle_.load(std::memory_order_acquire) & kFresh) == 0) return false;
        front_ = middle_.exchange(front_, std::memory_order_acq_rel) & kIndex;
        out = slots_[static_cast<std::size_t>(front_)];
        return true;
    }

private:
    static_assert(std::is_trivially_copyable_v<T>,
                  "LatestValue copies T on the real-time thread; T must not allocate or lock");
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
