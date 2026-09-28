// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/host/Handoff.h"

namespace bambi::host {

namespace {
std::atomic<int> gLive{0};
std::atomic<int> gFreedOnAudioThread{0};
thread_local bool tlInsideAudioBlock = false;
}  // namespace

SnapshotCensus::SnapshotCensus() noexcept { gLive.fetch_add(1, std::memory_order_relaxed); }

SnapshotCensus::~SnapshotCensus() {
    gLive.fetch_sub(1, std::memory_order_relaxed);
    if (tlInsideAudioBlock) gFreedOnAudioThread.fetch_add(1, std::memory_order_relaxed);
}

int SnapshotCensus::live() noexcept { return gLive.load(std::memory_order_relaxed); }
int SnapshotCensus::freedOnAudioThread() noexcept { return gFreedOnAudioThread.load(std::memory_order_relaxed); }

SnapshotCensus::InAudioBlock::InAudioBlock() noexcept { tlInsideAudioBlock = true; }
SnapshotCensus::InAudioBlock::~InAudioBlock() { tlInsideAudioBlock = false; }

}  // namespace bambi::host
