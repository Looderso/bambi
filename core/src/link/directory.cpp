// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/link/directory.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <type_traits>

#include "bambi/link/link.hpp"
#include "os.hpp"

namespace bambi {
namespace {

constexpr std::uint32_t kDirMagic = 0x41454452;  // 'AEDR'
constexpr std::uint32_t kDirVersion = 1;

/*  One listed session.
 *
 *  `ready` is 0 while the entry is being claimed or released, and otherwise the claim's number.
 *  A reader loads it on both sides of its copy: equal and non-zero means the copy belongs to
 *  one claim. A plain flag could not say that -- release, reclaim and a rewrite can all happen
 *  between two loads that both read 1.
 *
 *  `heartbeat` is 0 while an entry is claimed but not yet stamped, and lapsed entries are
 *  released by CAS'ing their heartbeat to 0 first. A refresh that lands in between makes the
 *  CAS fail, so a live session is never released by a reaper that looked a moment too early.
 */
struct Entry {
    std::atomic<std::uint32_t> occupied;
    std::atomic<std::uint32_t> ready;
    std::atomic<std::uint32_t> claims;  ///< numbers the claims, so `ready` never repeats
    std::atomic<std::uint32_t> reserved;
    std::atomic<std::uint64_t> heartbeat;  ///< moves forward only, while listed
    std::atomic<std::uint64_t> activity;   ///< moves forward only, while listed
    Uuid session;                          ///< written between the two stores of `ready`
};

struct Header {
    std::atomic<std::uint32_t> magic;
    std::atomic<std::uint32_t> version;
    std::uint32_t slotCount;
    std::uint32_t slotStride;
    std::uint32_t pad[4];
};

struct Segment {
    Header header;
    Entry entries[kLinkDirectorySlots];
};

static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "the session directory puts atomics in shared memory");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::is_trivially_copyable_v<Uuid>);

Segment* segmentOf(void* map) { return static_cast<Segment*>(map); }

void storeMax(std::atomic<std::uint64_t>& a, std::uint64_t v) {
    std::uint64_t cur = a.load(std::memory_order_acquire);
    while (cur < v && !a.compare_exchange_weak(cur, v, std::memory_order_acq_rel)) {}
}

struct Snapshot {
    Uuid session{};
    std::uint64_t heartbeat{0};
    std::uint64_t activity{0};
};

/// Copy a listed entry, all fields from one claim. False if unlisted or changed mid-copy.
bool read(const Entry& e, Snapshot& out) {
    if (e.occupied.load(std::memory_order_acquire) == 0) return false;
    const std::uint32_t before = e.ready.load(std::memory_order_acquire);
    if (before == 0) return false;
    out.session = e.session;
    out.heartbeat = e.heartbeat.load(std::memory_order_acquire);
    out.activity = e.activity.load(std::memory_order_acquire);
    std::atomic_thread_fence(std::memory_order_acquire);
    return e.ready.load(std::memory_order_acquire) == before;
}

/// Unlist an entry whose heartbeat was seen as `observed`. Fails if it was refreshed since.
void release(Entry& e, std::uint64_t observed) {
    if (!e.heartbeat.compare_exchange_strong(observed, 0, std::memory_order_acq_rel)) return;
    e.ready.store(0, std::memory_order_release);
    e.occupied.store(0, std::memory_order_release);
}

}  // namespace

LinkDirectory::LinkDirectory(std::string name) : name_(std::move(name)) {}

LinkDirectory::~LinkDirectory() { close(); }

bool LinkDirectory::open() {
    close();
    error_.clear();
    mapBytes_ = sizeof(Segment);
    if (!detail::mapShared(name_, mapBytes_, handle_, map_, error_)) return false;
    Segment* seg = segmentOf(map_);

    std::uint32_t expected = 0;
    if (seg->header.magic.compare_exchange_strong(expected, kDirMagic, std::memory_order_acq_rel)) {
        seg->header.slotCount = kLinkDirectorySlots;
        seg->header.slotStride = sizeof(Entry);
        seg->header.version.store(kDirVersion, std::memory_order_release);
        return true;
    }
    if (expected != kDirMagic) {
        error_ = "shared segment is not an bambi session directory";
        close();
        return false;
    }
    //  Made by someone else: wait for its layout to be published, then refuse a layout this
    //  build would misread -- the same reasoning as the bus (link.cpp, join).
    //  Short, for the reason mapShared gives: waiting here freezes the host's message thread.
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (seg->header.version.load(std::memory_order_acquire) != 0) break;
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    if (seg->header.version.load(std::memory_order_acquire) != kDirVersion || seg->header.slotStride != sizeof(Entry) ||
        seg->header.slotCount != kLinkDirectorySlots) {
        error_ = "session directory was created by an incompatible build";
        close();
        return false;
    }
    return true;
}

void LinkDirectory::close() {
    //  Entries are left listed: other instances of these sessions may be alive in other
    //  processes. An instance that knows its session is ending says so with remove().
    detail::unmapShared(map_, mapBytes_, handle_);
}

int LinkDirectory::find(const Uuid& session, std::uint64_t now) const {
    if (map_ == nullptr) return -1;
    const Segment* seg = segmentOf(map_);
    for (int i = 0; i < kLinkDirectorySlots; ++i) {
        Snapshot snap;
        if (read(seg->entries[i], snap) && snap.session == session && linkHeartbeatAlive(snap.heartbeat, now)) return i;
    }
    return -1;
}

int LinkDirectory::claim(const Uuid& session, std::uint64_t now) {
    Segment* seg = segmentOf(map_);

    //  Release lapsed entries first, so sessions that ended without saying so -- a crashed
    //  host -- do not fill the directory.
    for (auto& e : seg->entries) {
        if (e.occupied.load(std::memory_order_acquire) == 0) continue;
        const std::uint64_t hb = e.heartbeat.load(std::memory_order_acquire);
        if (hb != 0 && !linkHeartbeatAlive(hb, now)) release(e, hb);
    }

    for (int i = 0; i < kLinkDirectorySlots; ++i) {
        Entry& e = seg->entries[i];
        std::uint32_t free = 0;
        if (!e.occupied.compare_exchange_strong(free, 1, std::memory_order_acq_rel)) continue;
        e.ready.store(0, std::memory_order_release);
        e.heartbeat.store(now, std::memory_order_release);
        e.activity.store(0, std::memory_order_release);
        e.session = session;
        std::uint32_t number = e.claims.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (number == 0) number = 1;
        e.ready.store(number, std::memory_order_release);  // publish last
        return i;
    }
    return -1;
}

void LinkDirectory::stamp(const Uuid& session, std::uint64_t now, bool activity) {
    if (map_ == nullptr || session.isNil()) return;
    Segment* seg = segmentOf(map_);
    //  Two attempts: a release that races the stamp leaves the entry unlisted, and the second
    //  pass lists it again. Two instances of one session listing it at the same moment may
    //  produce two entries; list() merges them and the spare one lapses.
    for (int attempt = 0; attempt < 2; ++attempt) {
        int i = find(session, now);
        if (i < 0) i = claim(session, now);
        if (i < 0) return;  // full: unlisted until an entry lapses, never evicting a live one
        Entry& e = seg->entries[i];
        //  One window is left open on purpose. If this entry is released and claimed by another
        //  session between find() and these stores -- nanoseconds, and only while this session
        //  is lapsing or being removed -- the stores land on the newcomer. Its heartbeat merely
        //  runs a moment long; its activity could make it look recently used until the next
        //  real action. Closing the window needs a lock across processes, which is worse than one
        //  wrong guess the Link panel corrects.
        storeMax(e.heartbeat, now);
        if (activity) storeMax(e.activity, now);
        Snapshot snap;
        if (read(e, snap) && snap.session == session) return;
    }
}

void LinkDirectory::heartbeat(const Uuid& session, std::uint64_t now) { stamp(session, now, false); }

void LinkDirectory::touch(const Uuid& session, std::uint64_t now) { stamp(session, now, true); }

void LinkDirectory::remove(const Uuid& session) {
    if (map_ == nullptr) return;
    for (auto& e : segmentOf(map_)->entries) {
        Snapshot snap;
        if (!read(e, snap) || !(snap.session == session) || snap.heartbeat == 0) continue;
        release(e, snap.heartbeat);
    }
}

void LinkDirectory::list(std::vector<LinkSessionInfo>& out, std::uint64_t now) const {
    out.clear();
    if (map_ == nullptr) return;
    for (const auto& e : segmentOf(map_)->entries) {
        Snapshot snap;
        if (!read(e, snap) || !linkHeartbeatAlive(snap.heartbeat, now)) continue;
        const auto it =
            std::find_if(out.begin(), out.end(), [&](const LinkSessionInfo& s) { return s.session == snap.session; });
        if (it == out.end())
            out.push_back({snap.session, snap.activity});
        else
            it->lastActivity = std::max(it->lastActivity, snap.activity);
    }
    std::sort(out.begin(), out.end(), [](const LinkSessionInfo& a, const LinkSessionInfo& b) {
        if (a.lastActivity != b.lastActivity) return a.lastActivity > b.lastActivity;
        return a.session.bytes < b.session.bytes;  // deterministic when nobody has acted yet
    });
}

std::optional<Uuid> LinkDirectory::mostRecent(std::uint64_t now) const {
    std::vector<LinkSessionInfo> live;
    list(live, now);
    if (live.empty()) return std::nullopt;
    return live.front().session;
}

void LinkDirectory::unlink(const std::string& name) { detail::unlinkShared(name); }

Uuid sessionForNewInstance(const LinkDirectory& directory, std::uint64_t now, const Uuid& fresh) {
    if (const auto recent = directory.mostRecent(now)) return *recent;
    return fresh;
}

}  // namespace bambi
