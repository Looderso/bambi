// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdio>
#include <exception>

namespace bambi {

/// A tool's or a check's whole run, with one place for whatever escapes it: a message on stderr and a failure,
/// never an abort. `name` says whose message it is.
template <class Body>
int guarded(const char* name, Body&& body) noexcept {
    try {
        return body();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s: %s\n", name, e.what());
    } catch (...) {
        std::fprintf(stderr, "%s: failed\n", name);
    }
    return 1;
}

}  // namespace bambi
