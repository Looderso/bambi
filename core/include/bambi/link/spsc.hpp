// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace bambi {

/*  A bounded queue from one producer to one consumer, wait-free on both sides: for what LatestValue cannot do, hand
 *  over every item rather than only the newest. The plugin's audio thread retires each engine snapshot it replaces,
 *  and the message thread frees it, since freeing on the audio thread is what this queue exists to avoid.
 *
 *  push() refuses when full rather than overwriting, and says so; the producer decides what that means -- the
 *  plugin keeps its current snapshot until there is room. "One producer" means one at a time, not one thread
 *  forever: a host may call the audio callback from different threads, but never from two at once.
 */
template <typename T, std::size_t Capacity>
class SpscQueue {
public:
    /// Producer only.
    bool push(const T& value) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head - tail_.load(std::memory_order_acquire) == Capacity) return false;
        slots_[head & (Capacity - 1)] = value;
        head_.store(head + 1, std::memory_order_release);  // publish only after the write
        return true;
    }

    /// Producer only: whether push() would be refused now. The consumer can only make room.
    bool full() const noexcept {
        return head_.load(std::memory_order_relaxed) - tail_.load(std::memory_order_acquire) == Capacity;
    }

    /// Consumer only.
    bool pop(T& out) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        out = slots_[tail & (Capacity - 1)];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

private:
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "push() copies T on a real-time thread");
    static_assert(std::atomic<std::size_t>::is_always_lock_free);

    std::array<T, Capacity> slots_{};
    std::atomic<std::size_t> head_{0};  ///< written by the producer
    std::atomic<std::size_t> tail_{0};  ///< written by the consumer
};

}  // namespace bambi
