// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <new>

#include "bambi/editor/CheckKit.h"
#include "bambi/host/TestPlayHead.h"

/*  Counts what the thread rendering a block allocates and frees, so a check can say a block does neither. It replaces the
 *  allocator for the whole program, so a check program includes this from its one main file and from nowhere else.
 *  Only the thread that asked is counted: the link timer and the message thread allocate freely while a block renders,
 *  and must. JUCE's own buffers allocate with malloc, not new: with glibc, malloc itself is replaced and counted; macOS
 *  and Windows offer no way to do that from inside a program, so there only operator new is counted, and the check
 *  says so. No thread_local: its first use on a thread may itself allocate. */
#include <atomic>
#include <thread>
#if JUCE_WINDOWS
#include <malloc.h>
#endif

namespace bambi::editor::detail {
inline std::atomic<bool> gCounting{false};
inline std::atomic<long> gAllocations{0};
inline std::atomic<std::thread::id> gCounted{};
inline void counted() noexcept {
    if (gCounting.load(std::memory_order_acquire) &&
        std::this_thread::get_id() == gCounted.load(std::memory_order_relaxed))
        gAllocations.fetch_add(1, std::memory_order_relaxed);
}
}  // namespace bambi::editor::detail

#if JUCE_LINUX
extern "C" void* __libc_malloc(std::size_t);
extern "C" void* __libc_calloc(std::size_t, std::size_t);
extern "C" void* __libc_realloc(void*, std::size_t);
extern "C" void __libc_free(void*);
extern "C" void* malloc(std::size_t n) noexcept {
    bambi::editor::detail::counted();
    return __libc_malloc(n);
}
extern "C" void* calloc(std::size_t k, std::size_t n) noexcept {
    bambi::editor::detail::counted();
    return __libc_calloc(k, n);
}
extern "C" void* realloc(void* p, std::size_t n) noexcept {
    bambi::editor::detail::counted();
    return __libc_realloc(p, n);
}
extern "C" void free(void* p) noexcept {
    if (p != nullptr) bambi::editor::detail::counted();
    __libc_free(p);
}
namespace bambi::editor::detail {
inline constexpr bool kCountsMalloc = true;
}
#else
void* operator new(std::size_t n) {
    bambi::editor::detail::counted();
    if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept {
    if (p != nullptr) bambi::editor::detail::counted();
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { operator delete(p); }
void* operator new(std::size_t n, std::align_val_t a) {
    bambi::editor::detail::counted();
    const auto align = static_cast<std::size_t>(a);
#if JUCE_WINDOWS
    if (void* p = _aligned_malloc(n == 0 ? 1 : n, align)) return p;
#else
    void* p = nullptr;
    if (posix_memalign(&p, std::max(align, sizeof(void*)), n == 0 ? 1 : n) == 0) return p;
#endif
    throw std::bad_alloc();
}
void* operator new[](std::size_t n, std::align_val_t a) { return operator new(n, a); }
void operator delete(void* p, std::align_val_t) noexcept {
    if (p != nullptr) bambi::editor::detail::counted();
#if JUCE_WINDOWS
    _aligned_free(p);
#else
    std::free(p);
#endif
}
void operator delete(void* p, std::size_t, std::align_val_t a) noexcept { operator delete(p, a); }
void operator delete[](void* p, std::align_val_t a) noexcept { operator delete(p, a); }
void operator delete[](void* p, std::size_t, std::align_val_t a) noexcept { operator delete(p, a); }
namespace bambi::editor::detail {
inline constexpr bool kCountsMalloc = false;
}
#endif

namespace bambi::editor {

/// How often `work` allocated or freed on this thread.
template <class F>
long allocationsDuring(F&& work) {
    detail::gCounted.store(std::this_thread::get_id(), std::memory_order_relaxed);
    detail::gAllocations.store(0);
    detail::gCounting.store(true, std::memory_order_release);
    work();
    detail::gCounting.store(false, std::memory_order_release);
    return detail::gAllocations.load();
}

/*  A second of blocks at layout `l`, after a few to settle, allocates nothing on the thread rendering them: the
    audio thread's rule. `what` names the layout in the check's line. */
template <class Processor>
void checkBlocksAllocateNothing(const Layout& l, const juce::String& what) {
    //  The instrument first: a counter that cannot see malloc would pass every block.
    if (detail::kCountsMalloc) {
        void* volatile probe = nullptr;
        check(allocationsDuring([&] { probe = std::malloc(64); }) > 0, what + ": a malloc on this thread is seen");
        std::free(probe);
    } else {
        std::printf("  (only operator new is counted on this platform; JUCE's malloc is not)\n");
    }
    Processor proc;
    if (!proc.setBusesLayout(l)) {
        check(false, what + ": the layout is accepted");
        return;
    }
    constexpr int block = 128;
    host::TestPlayHead head;
    proc.setPlayHead(&head);
    proc.setRateAndBufferSizeDetails(kCheckRate, block);
    proc.prepareToPlay(kCheckRate, block);
    juce::AudioBuffer<float> buffer(std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels()), block);
    juce::MidiBuffer midi;
    juce::int64 at = 0;
    const auto render = [&] {
        head.time = at;
        for (int c = 0; c < proc.getTotalNumInputChannels(); ++c)
            for (int i = 0; i < block; ++i) buffer.setSample(c, i, fieldSample(c, at + i));
        proc.processBlock(buffer, midi);
        at += block;
    };
    for (int i = 0; i < 8; ++i) render();  // the first snapshot adopted, the first control steps taken
    const long allocated = allocationsDuring([&] {
        for (int i = 0; i < static_cast<int>(kCheckRate) / block; ++i) render();
    });
    //  Catches: a bus taken through getBusBuffer, which allocates its channel pointers from 32 channels.
    check(allocated == 0, what + ": a second of blocks allocates and frees nothing (" + juce::String(allocated) + ")");
}

}  // namespace bambi::editor
