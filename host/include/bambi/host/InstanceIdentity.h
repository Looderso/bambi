// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <mutex>
#include <string>

#include "bambi/patch/identity.hpp"

/*  Who this instance is, held for every plugin: the session, the instance id, the user's label.
 *
 *  Read from the message thread and from whichever thread a host saves state on, so it is held
 *  under a lock and handed out by value. Never touched by an audio callback.
 */
namespace bambi::host {

class InstanceIdentity {
public:
    Identity get() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return identity_;
    }

    /// The user's own name for the instance; empty gives it back to the track's.
    void rename(const std::string& label) {
        const std::lock_guard<std::mutex> lock(mutex_);
        identity_.label = label;
    }

    /// The link node has decided: from here the session and the id are this instance's, whatever a later state load carries.
    void adopt(const Uuid& session, const Uuid& instance) {
        const std::lock_guard<std::mutex> lock(mutex_);
        identity_.session = session;
        identity_.instance = instance;
        adopted_ = true;
    }

    /*  A state has been loaded. Its identity is taken only before this instance has joined: a
        project loading, a track being duplicated. State loaded into a running instance -- a preset,
        the host's undo -- must not move it to another session or hand it another instance's id.
        Until the join, the last state loaded is the one that counts. */
    void fromState(const Identity& loaded) {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!adopted_) identity_ = loaded;
    }

private:
    mutable std::mutex mutex_;
    Identity identity_;
    bool adopted_{false};
};

}  // namespace bambi::host
