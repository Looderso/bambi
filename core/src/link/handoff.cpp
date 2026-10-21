// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/link/handoff.hpp"

#include <array>
#include <cstddef>
#include <functional>
#include <thread>

namespace bambi {

namespace {
std::atomic<int> gLive{0};
std::atomic<int> gFreedOnAudioThread{0};

//  The threads inside an audio block now, each by a hash of its id, 0 a free place. A table and not a thread_local:
//  macOS allocates a thread_local's storage the first time a thread touches it, which on an audio thread is exactly
//  what is counted. More threads in blocks at once than places goes uncounted, never wrong.
constexpr std::size_t kBlockThreads = 64;
std::array<std::atomic<std::size_t>, kBlockThreads> gInBlock{};

std::size_t self() noexcept {
    const std::size_t h = std::hash<std::thread::id>{}(std::this_thread::get_id());
    return h == 0 ? 1 : h;
}

bool insideBlock() noexcept {
    const std::size_t me = self();
    for (const auto& place : gInBlock)
        if (place.load(std::memory_order_acquire) == me) return true;
    return false;
}
}  // namespace

SnapshotCensus::SnapshotCensus() noexcept { gLive.fetch_add(1, std::memory_order_relaxed); }

SnapshotCensus::~SnapshotCensus() {
    gLive.fetch_sub(1, std::memory_order_relaxed);
    if (insideBlock()) gFreedOnAudioThread.fetch_add(1, std::memory_order_relaxed);
}

int SnapshotCensus::live() noexcept { return gLive.load(std::memory_order_relaxed); }
int SnapshotCensus::freedOnAudioThread() noexcept { return gFreedOnAudioThread.load(std::memory_order_relaxed); }

SnapshotCensus::InAudioBlock::InAudioBlock() noexcept {
    const std::size_t me = self();
    for (std::size_t i = 0; i < kBlockThreads; ++i) {
        std::size_t free = 0;
        if (gInBlock[i].compare_exchange_strong(free, me, std::memory_order_acq_rel)) {
            slot_ = static_cast<int>(i);
            return;
        }
    }
}

SnapshotCensus::InAudioBlock::~InAudioBlock() {
    if (slot_ >= 0) gInBlock[static_cast<std::size_t>(slot_)].store(0, std::memory_order_release);
}

}  // namespace bambi
