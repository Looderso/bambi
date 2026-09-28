// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

/*  The render tools' timed changes: `--at S PATH=VALUE` sets a setting S seconds in, `--at S reset`
 *  resets the engine there. A render applies them at control-hop boundaries, in time order. */
namespace bambi::tools {

struct Change {
    double at{0.0};  ///< seconds
    std::string path;
    std::string value;
    bool reset() const { return path == "reset"; }
};

/// Splits "PATH=VALUE"; false when there is no '='.
inline bool splitSetting(const std::string& kv, std::string& path, std::string& value) {
    const auto eq = kv.find('=');
    if (eq == std::string::npos) return false;
    path = kv.substr(0, eq);
    value = kv.substr(eq + 1);
    return true;
}

/// A change at `at` seconds from "PATH=VALUE" or "reset"; false when `what` is neither.
inline bool parseChange(double at, const std::string& what, Change& out) {
    out.at = at;
    if (what == "reset") {
        out.path = "reset";
        return true;
    }
    return splitSetting(what, out.path, out.value);
}

/// Changes in time order, handed out as a render reaches them.
class ChangeSchedule {
public:
    explicit ChangeSchedule(std::vector<Change> changes) : changes_(std::move(changes)) {
        std::stable_sort(changes_.begin(), changes_.end(),
                         [](const Change& a, const Change& b) { return a.at < b.at; });
    }

    /// Applies every change due by sample `pos` at rate `fs`; returns the first one `apply` refuses, or null.
    template <class Apply>
    const Change* applyDue(double fs, std::size_t pos, Apply&& apply) {
        while (next_ < changes_.size() && changes_[next_].at * fs <= static_cast<double>(pos)) {
            const Change& c = changes_[next_++];
            if (!apply(c)) return &c;
        }
        return nullptr;
    }

private:
    std::vector<Change> changes_;
    std::size_t next_{0};
};

}  // namespace bambi::tools
