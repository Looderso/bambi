// SPDX-License-Identifier: GPL-3.0-or-later
#include "Controls.h"

#include <algorithm>
#include <string>

#include "bambi/reverb/control.hpp"
#include "bambi/reverb/images.hpp"
#include "bambi/reverb/room.hpp"

namespace bambi::ui {

ReverbControlState::ReverbControlState(BambiReverbProcessor& p)
    : bambi::editor::EffectControls(p, bambi::reverbParams(), bambi::reverbMod(), 3,
                                    {.firstTab = 1, .slots = {{0, "region1", "send"}, {1, "region2", "return"}}}),
      processor(p) {}

bambi::ParamId ReverbControlState::param(const char* key) const {
    return static_cast<bambi::ParamId>(bambi::reverbParams().byKey(key));
}

bambi::ParamId ReverbControlState::slotParam(int which, const char* field) const {
    const auto key = "region" + std::to_string(std::clamp(which, 0, 1) + 1) + "." + field;
    return static_cast<bambi::ParamId>(bambi::reverbParams().byKey(key));
}

/*  Point at a direction and Reverb answers what would be added for a source there -- a mark per
    reflection where it arrives from, at its weight, computed from the parameters, so it costs the
    audio thread nothing.

    The room and the reflections are `reverb/images.hpp`'s, which the engine uses: the picture
    cannot describe a room the plugin is not in. Only the part heard as a reflection is drawn --
    what the walls scattered goes into the tail and has no direction to be drawn from. */
void ReverbControlState::answerProbe(bambi::Vec3 from, bambi::ui::ProbeReply& into) {
    const auto settings = bambi::reverbSettingsFrom(
        bambi::reverbParams(), patch.params, static_cast<bambi::RoomShape>(std::clamp(patch.room.shape, 0, 2)),
        patch.regions[0].shape, patch.regions[1].shape, patch.renderQuality);
    bambi::ReverbResolver resolver;
    const auto frame = resolver.resolve(settings, false, processor.diagnostics().sampleRate.load());

    /*  The same room the engine derives, from the same settings: `deriveRoom` is what the engine
        calls. */
    const bambi::Room room = bambi::deriveRoom(frame.room);
    std::array<bambi::Reflection, bambi::kMaxImages> reflections{};
    const int count = bambi::reflectionsOf(room, from, frame.distance, reflections);
    into.numbered = false;  // the reflections are one answer, what the room adds
    for (int i = 0; i < count; ++i) {
        const auto& r = reflections[static_cast<std::size_t>(i)];
        if (r.mirror <= 0.0) continue;
        /*  Each its own group, so nothing is joined by a line: reflections did not come one from
            another the way Echo's passes did, and a path through them would say they had. */
        into.marks.push_back({r.direction, r.mirror, 0.0, i, true});
    }
}

}  // namespace bambi::ui
