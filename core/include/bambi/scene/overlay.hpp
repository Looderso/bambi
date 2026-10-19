// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "bambi/patch/identity.hpp"

/*  What a scene draws of the other instances on the bus, and which of it the user has switched off. One master -- the
 *  scene's toggle -- and a choice per item: an instance, or a part of one (a decoder's speakers, its head). The choices
 *  are the window's own and never a parameter: nothing audible reads them, and they are not saved with the session.
 *
 *  An item without a choice of its own follows `defaultOn`, so a newcomer on the bus appears or stays hidden by the
 *  overlay's rule and nobody has to be asked. The master off hides everything and forgets nothing: the choices are
 *  there again when it is switched back on.
 */
namespace bambi {

/// One thing an overlay can show: an instance, or one of its parts (0 is the instance itself).
struct OverlayKey {
    Uuid instance{};
    std::uint8_t part{0};
    friend bool operator==(const OverlayKey&, const OverlayKey&) = default;
};

class OverlaySelection {
public:
    explicit OverlaySelection(bool defaultOn = true) : defaultOn_(defaultOn) {}

    bool master() const { return master_; }
    void setMaster(bool on) { master_ = on; }

    /// Whether the item's own box is ticked: what the menu shows, whatever the master says.
    bool chosen(const OverlayKey& key) const {
        for (const auto& c : choices_)
            if (c.first == key) return c.second;
        return defaultOn_;
    }
    /// Whether the scene draws it: the master is on and the item is chosen.
    bool drawn(const OverlayKey& key) const { return master_ && chosen(key); }

    void choose(const OverlayKey& key, bool on) {
        for (auto& c : choices_)
            if (c.first == key) {
                c.second = on;
                return;
            }
        choices_.emplace_back(key, on);
    }
    /// Every one of `keys`, on or off: the menu's all and none.
    void chooseAll(std::span<const OverlayKey> keys, bool on) {
        for (const auto& key : keys) choose(key, on);
    }
    /// Drops the choices of whatever is no longer in `present`, so a list of departed instances never grows.
    void forget(std::span<const OverlayKey> present) {
        std::erase_if(choices_, [&](const std::pair<OverlayKey, bool>& c) {
            for (const auto& key : present)
                if (key == c.first) return false;
            return true;
        });
    }
    std::size_t choices() const { return choices_.size(); }
    bool defaultOn() const { return defaultOn_; }

private:
    bool master_{true};
    bool defaultOn_{true};
    std::vector<std::pair<OverlayKey, bool>> choices_;
};

}  // namespace bambi
