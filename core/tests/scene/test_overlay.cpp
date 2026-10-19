// SPDX-License-Identifier: GPL-3.0-or-later
#include <vector>

#include "../doctest.h"
#include "bambi/scene/overlay.hpp"

namespace bambi {

namespace {
OverlayKey keyOf(int n, std::uint8_t part = 0) {
    OverlayKey k;
    k.instance.bytes[0] = static_cast<std::uint8_t>(n);
    k.part = part;
    return k;
}
}  // namespace

TEST_CASE("an item follows the overlay's default until it is chosen, and a choice is its own") {
    //  Catches: a default that is ignored (a newcomer hidden on an overlay that shows what arrives), or a choice
    //  that spills onto other items or onto another part of the same instance.
    OverlaySelection on(true), off(false);
    CHECK(on.chosen(keyOf(1)));
    CHECK_FALSE(off.chosen(keyOf(1)));

    on.choose(keyOf(1), false);
    CHECK_FALSE(on.chosen(keyOf(1)));
    CHECK(on.chosen(keyOf(2)));
    CHECK(on.chosen(keyOf(1, 1)));  // the same instance's speakers are a separate item

    off.choose(keyOf(3, 2), true);
    CHECK(off.chosen(keyOf(3, 2)));
    CHECK_FALSE(off.chosen(keyOf(3, 1)));
    CHECK_FALSE(off.chosen(keyOf(4)));

    on.choose(keyOf(1), true);  // chosen again: one entry, not two
    CHECK(on.chosen(keyOf(1)));
    CHECK(on.choices() == 1);
}

TEST_CASE("the master hides everything and forgets nothing") {
    //  Catches: the master read only for the toggle (an item drawn with it off), or the choices cleared when it
    //  goes off, so that switching it back on shows everything again.
    OverlaySelection s(true);
    s.choose(keyOf(1), false);
    CHECK(s.drawn(keyOf(2)));
    CHECK_FALSE(s.drawn(keyOf(1)));

    s.setMaster(false);
    CHECK_FALSE(s.drawn(keyOf(2)));
    CHECK(s.chosen(keyOf(2)));  // its box still shows what was chosen
    CHECK_FALSE(s.chosen(keyOf(1)));

    s.choose(keyOf(1), true);  // a tick with the master off changes the choice and does not wake the master
    CHECK_FALSE(s.master());
    CHECK_FALSE(s.drawn(keyOf(1)));

    s.setMaster(true);
    CHECK(s.drawn(keyOf(1)));
    CHECK(s.drawn(keyOf(2)));
}

TEST_CASE("all and none set every listed item, and only those") {
    //  Catches: all or none reaching an item that is not in the list, or leaving one out.
    OverlaySelection s(true);
    const std::vector<OverlayKey> listed{keyOf(1), keyOf(2), keyOf(3)};
    s.chooseAll(listed, false);
    for (const auto& k : listed) CHECK_FALSE(s.chosen(k));
    CHECK(s.chosen(keyOf(9)));  // not listed: follows the default

    s.chooseAll(listed, true);
    for (const auto& k : listed) CHECK(s.chosen(k));
}

TEST_CASE("an instance that has left takes its choice with it") {
    //  Catches: choices kept for ever (the list grows with every instance a session ever had), or forget() dropping
    //  the choice of one that is still there.
    OverlaySelection s(true);
    s.choose(keyOf(1), false);
    s.choose(keyOf(2), false);
    s.choose(keyOf(3), false);
    const std::vector<OverlayKey> present{keyOf(1), keyOf(3)};
    s.forget(present);
    CHECK(s.choices() == 2);
    CHECK_FALSE(s.chosen(keyOf(1)));
    CHECK_FALSE(s.chosen(keyOf(3)));
    CHECK(s.chosen(keyOf(2)));  // gone: back to the default, should it return
}

}  // namespace bambi
