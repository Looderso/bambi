// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "bambi/patch/identity.hpp"

namespace bambi {

/*  The live sessions on this machine -- the one thing a freshly inserted instance cannot learn from its host, since
 *  it has no saved state to restore a session from. POSIX shared memory cannot be enumerated on macOS, so sessions
 *  list themselves here: one small machine-wide segment, one entry per live session, kept fresh by that session's
 *  instances and lapsing like a bus slot when they all go quiet. A new instance joins the session most recently
 *  active; the Link panel moves it when that guess is wrong.
 *
 *  Heartbeat and activity are tracked separately, so a background project merely playing cannot look like the most
 *  recently used one; only a user action counts as activity. Several processes write one entry at once, so fields
 *  that change after registration only move forward, and an entry is read only inside a check that its claim did
 *  not change during the copy. Message thread only.
 */

inline constexpr int kLinkDirectorySlots = 64;
inline constexpr const char* kLinkDirectoryName = "/bambi.directory.1";

struct LinkSessionInfo {
    Uuid session{};
    std::uint64_t lastActivity{0};  ///< linkNowMicros of the latest user action; 0 = none yet
};

class LinkDirectory {
public:
    /// `name` exists for tests, which must never share the machine's real directory.
    explicit LinkDirectory(std::string name = kLinkDirectoryName);
    ~LinkDirectory();
    LinkDirectory(const LinkDirectory&) = delete;
    LinkDirectory& operator=(const LinkDirectory&) = delete;

    /*  Maps the directory, creating it if this is the first instance on the machine. False and error() set if
     *  shared memory is unavailable or the directory was made by an incompatible build; every other method is then
     *  a safe no-op. */
    bool open();
    void close();
    bool isOpen() const { return map_ != nullptr; }
    const std::string& error() const { return error_; }

    void heartbeat(const Uuid& session, std::uint64_t now);  ///< keeps `session` listed, without activity
    void touch(const Uuid& session, std::uint64_t now);      ///< the user did something in `session`

    void remove(const Uuid& session);  ///< unlists at once, so a project started right after does not join it

    void list(std::vector<LinkSessionInfo>& out, std::uint64_t now) const;  ///< most recently active first; allocates

    std::optional<Uuid> mostRecent(std::uint64_t now) const;  ///< the live session with the most recent activity

    static void unlink(const std::string& name);  ///< removes a directory from the system; tests only

private:
    int find(const Uuid& session, std::uint64_t now) const;
    int claim(const Uuid& session, std::uint64_t now);
    void stamp(const Uuid& session, std::uint64_t now, bool activity);

    std::string name_, error_;
    void* map_{nullptr};
    std::size_t mapBytes_{0};
    std::intptr_t handle_{-1};  ///< the OS's handle on the segment
};

/// The session a new instance joins: the most recently active live session, or `fresh` when none is live.
Uuid sessionForNewInstance(const LinkDirectory& directory, std::uint64_t now, const Uuid& fresh);

}  // namespace bambi
