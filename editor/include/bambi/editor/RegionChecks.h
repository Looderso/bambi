// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "bambi/editor/PluginEditor.h"
#include "bambi/host/PluginProcessor.h"
#include "bambi/host/TestPlayHead.h"
#include "bambi/ui/RegionOverlay.h"

/*  The region checks, written once and run by every plugin's check suite. The handles, the grip and
 *  the write-back are shared code, so what is checked is the same in all three; the checks drive the
 *  equirect through the mouse, as a person does, and ask it what it drew (`ui::RegionView`). What
 *  differs is only how a plugin opens its region's page, and whether it has a ping.
 */
namespace bambi::editor {

struct RegionCheckSpec {
    /// Open or close the page whose region takes handles: the encoder's source page, an effect's tab.
    std::function<void(PluginEditor&, bool open)> showRegion;
    /// Where the ping is pointed, for a plugin that has one.
    std::function<std::optional<Vec3>(PluginEditor&)> ping{};
};

/// Where a region's handles stand, and which a press takes: pure geometry, no plugin needed.
inline void checkRegionHandleGeometry(const std::function<void(bool, const juce::String&)>& check) {
    using ui::RegionHandle;
    using K = RegionHandle::Kind;

    Region spot;
    spot.kind = RegionKind::Spot;
    spot.size = 0.6;
    spot.yaw = 0.7;
    spot.pitch = 0.3;
    std::vector<RegionHandle> handles;
    ui::regionHandles(spot, handles);
    check(handles.size() == 2, "regions: a spot has two handles, its edge and its aim");
    //  Catches: an edge placed at a fixed angle, or at the wrong one: it is exactly `size` from the aim.
    const auto aim = aimOf(spot);
    check(handles[0].kind == K::Edge &&
              std::abs(std::acos(std::clamp(dot(handles[0].direction, aim), -1.0, 1.0)) - spot.size) < 1e-9,
          "regions: the edge handle sits exactly `size` from the aim");
    check(handles.back().kind == K::Aim && dot(handles.back().direction, aim) > 0.999999,
          "regions: and the aim is registered LAST");

    Region every;
    every.kind = RegionKind::Everywhere;
    std::vector<RegionHandle> none;
    ui::regionHandles(every, none);
    check(none.empty(), "regions: everywhere has no handles, nothing to aim and nothing to size");

    //  Catches: a roll handle offered where a turn about the axis cannot be seen.
    const auto hasRoll = [](int dots) {
        Region r;
        r.kind = RegionKind::Dots;
        r.dots = dots;
        std::vector<RegionHandle> hs;
        ui::regionHandles(r, hs);
        return std::any_of(hs.begin(), hs.end(), [](const RegionHandle& h) { return h.kind == K::Roll; });
    };
    check(!hasRoll(2) && hasRoll(6), "regions: two dots show no roll handle; six do");

    //  The hit radii, 13 for the aim and 12 for the rest, on a spot facing the viewer.
    const Camera camera;
    const Viewport vp{200.0, 200.0, 150.0, 150.0};
    const auto screenOf = [&](Vec3 d) {
        const auto s = toScreen(Projection::Globe, camera, vp, d);
        return juce::Point<float>{static_cast<float>(s.x), static_cast<float>(s.y)};
    };
    Region face;
    face.kind = RegionKind::Spot;
    face.size = 0.6;
    std::vector<RegionHandle> faceHandles;
    ui::regionHandles(face, faceHandles);
    const auto aimPixel = screenOf(faceHandles.back().direction);
    const auto edgePixel = screenOf(faceHandles[0].direction);
    const auto hit = [&](juce::Point<float> at) {
        return ui::regionHandleAt(Projection::Globe, camera, vp, faceHandles, at);
    };
    //  Catches: the radii swapped, or one standing for both: 12.5 px is inside 13 and outside 12.
    check(hit(aimPixel.translated(12.5f, 0.0f)) == static_cast<int>(faceHandles.size()) - 1,
          "regions: the aim is hit at 12.5 px, its radius is 13");
    check(hit(aimPixel.translated(13.5f, 0.0f)) < 0, "regions: and missed at 13.5");
    check(hit(edgePixel.translated(11.5f, 0.0f)) == 0, "regions: an edge handle is hit at 11.5 px, its radius is 12");
    check(hit(edgePixel.translated(12.5f, 0.0f)) < 0, "regions: and missed at 12.5");

    /*  Catches: a handle on top of the aim not being taken first -- which registering the aim last
        achieves only if the hit-test walks forward. A zero-size spot puts its edge on its aim, so
        only the order decides. */
    Region pinched;
    pinched.kind = RegionKind::Spot;
    pinched.size = 0.0;
    std::vector<RegionHandle> onTop;
    ui::regionHandles(pinched, onTop);
    check(ui::regionHandleAt(Projection::Globe, camera, vp, onTop, screenOf(onTop.back().direction)) == 0,
          "regions: where a handle sits ON the aim, the handle is taken and not the aim");
}

/*  The plugin's window: its region's handles in the equirect, driven by the mouse and written back
    through its parameters. */
template <class Processor>
void checkRegions(const RegionCheckSpec& spec, const std::function<void(bool, const juce::String&)>& check) {
    using ui::RegionHandle;
    using K = RegionHandle::Kind;

    checkRegionHandleGeometry(check);

    /*  Playing, a block at a time: a rate set to restart -- how it opens -- turns only while the transport
        runs. A head that stood still would read as a locate every block and take the turn back. */
    host::TestPlayHead head;

    Processor proc;
    host::PluginProcessor& base = proc;
    proc.setPlayHead(&head);
    proc.prepareToPlay(48000.0, 128);
    std::unique_ptr<juce::AudioProcessorEditor> window(proc.createEditor());
    auto* editor = dynamic_cast<PluginEditor*>(window.get());
    auto* area = editor != nullptr ? editor->equirectArea() : nullptr;
    auto* view = editor != nullptr ? editor->equirectRegions() : nullptr;
    if (area == nullptr || view == nullptr)
        return check(false, "regions: the plugin's window is the shared one, with a region view");

    const auto& m = base.linkHost().manifest();
    const auto param = [&](const char* key) { return base.hostParameter(static_cast<ParamId>(m.byKey(key))); };
    const auto live = [&](const char* key) {
        auto* p = param(key);
        return p == nullptr ? -999.0f : p->convertFrom0to1(p->getValue());
    };
    const auto set = [&](const char* key, float v) {
        if (auto* p = param(key)) p->setValueNotifyingHost(p->convertTo0to1(v));
    };
    const auto kind = [&](RegionKind k, int dots = 6) {
        base.document().edit("region kind", [k, dots](PluginState& st) {
            st.regions[0].shape.kind = k;
            st.regions[0].shape.dots = dots;
        });
    };
    const auto frame = [&] {
        editor->tick();
        editor->paintNow();
    };
    const auto handle = [&](K which) -> std::optional<RegionHandle> {
        frame();
        for (const auto& h : view->handles())
            if (h.kind == which) return h;
        return std::nullopt;
    };
    const auto play = [&](int blocks) {
        const int channels = std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
        juce::AudioBuffer<float> buffer(channels, 128);
        juce::MidiBuffer midi;
        for (int b = 0; b < blocks; ++b) {
            buffer.clear();
            proc.processBlock(buffer, midi);
            head.time += 128;
        }
    };

    kind(RegionKind::Spot);
    spec.showRegion(*editor, true);
    const auto aim = handle(K::Aim);
    check(aim.has_value(), "regions: with the region's page open, it has handles in the scene");
    if (!aim) return;

    //  ---- carried by the mouse, written through the parameters ------------------------------------
    //  The aim to 40 deg left: yaw is counter-clockwise from front. Catches: a sign flip and a units mix.
    area->dragBetween(view->pixelOf(*aim), view->screenOf(fromAzEl(40.0 * kDeg2Rad, 0.0)));
    check(std::abs(live("region1.yaw") - 40.0f) < 1.0f,
          "regions: the aim dragged to 40 deg left writes yaw 40 (" + juce::String(live("region1.yaw"), 1) + ")");
    //  The edge to 30 deg above that aim sets size 30, whatever the aim is.
    if (const auto edge = handle(K::Edge))
        area->dragBetween(view->pixelOf(*edge), view->screenOf(fromAzEl(40.0 * kDeg2Rad, 30.0 * kDeg2Rad)));
    check(
        std::abs(live("region1.size") - 30.0f) < 1.0f,
        "regions: the edge dragged 30 deg from the aim writes size 30 (" + juce::String(live("region1.size"), 1) + ")");
    if (const auto a = handle(K::Aim)) {
        area->clickAt(view->pixelOf(*a));  // a double-click is a press first, as the mouse delivers it
        area->doubleClickAt(view->pixelOf(*a));
    }
    check(std::abs(live("region1.yaw")) < 0.01f, "regions: a double-click on the aim puts it back to front");

    //  Catches: a pointer dragged far off the map losing the aim instead of carrying it to the edge.
    if (const auto a = handle(K::Aim))
        area->dragBetween(view->pixelOf(*a), view->pixelOf(*a).translated(-2000.0f, 0.0f));
    check(std::abs(live("region1.yaw")) > 170.0f,
          "regions: dragged far off the map, the aim follows to its edge (yaw " + juce::String(live("region1.yaw"), 1) +
              ")");
    set("region1.yaw", 0.0f);

    /*  ---- pressed where it is drawn, a handle moves nothing --------------------------------------
        Out past the slop and back, so it is a drag that ends where it began. Catches: the roll measured
        against the handle's distance from the aim (a 45 deg jump), the sectors' fill with the roll taken
        off twice, and a dot's size measured from the aim instead of from the dot it sizes. */
    const auto inPlace = [&](K which) {
        const auto h = handle(which);
        if (!h) return false;
        const auto at = view->pixelOf(*h);
        area->pressAt(at);
        area->moveTo(at.translated(8.0f, 0.0f));
        area->moveTo(at);
        area->releaseDrag();
        frame();
        return true;
    };
    kind(RegionKind::Sectors);
    set("region1.roll", 30.0f);
    set("region1.fill", 0.3f);
    check(inPlace(K::Roll) && std::abs(live("region1.roll") - 30.0f) < 0.2f,
          "regions: sectors, the roll handle pressed and let go where it is leaves roll at 30 (" +
              juce::String(live("region1.roll"), 1) + ")");
    check(inPlace(K::Edge) && std::abs(live("region1.fill") - 0.3f) < 0.005f,
          "regions: and the edge, with roll 30, leaves fill at 0.3 (" + juce::String(live("region1.fill"), 3) + ")");
    kind(RegionKind::Dots, 6);
    set("region1.roll", 20.0f);
    //  30 deg, so the size handle stands clear of the roll handle's 12 px on the dot it sizes
    set("region1.dot_size", 30.0f);
    check(inPlace(K::Roll) && std::abs(live("region1.roll") - 20.0f) < 0.2f,
          "regions: six dots, the roll handle leaves roll at 20 (" + juce::String(live("region1.roll"), 1) + ")");
    check(inPlace(K::Edge) && std::abs(live("region1.dot_size") - 30.0f) < 0.2f,
          "regions: and the size handle leaves dot size at 30 (" + juce::String(live("region1.dot_size"), 1) + ")");
    set("region1.roll", 0.0f);

    //  ---- a band at rest: its axis is straight up, the map's whole top edge -----------------------
    //  The aim sits at the yaw its angles give. Every band, sectors and dots region starts here.
    kind(RegionKind::Band);
    set("region1.yaw", 40.0f);
    if (const auto a = handle(K::Aim)) {
        const auto az = azimuth(a->shownIn(Projection::Equirect)) * kRad2Deg;
        check(std::abs(az - 40.0) < 0.5,
              "regions: at a pole, the aim is drawn at its yaw on the map (" + juce::String(az, 1) + " deg)");
        area->pressAt(view->pixelOf(*a));
        area->moveTo(view->pixelOf(*a).translated(0.0f, 30.0f));
        area->releaseDrag();
        check(std::abs(live("region1.pitch")) > 1.0f, "regions: and a press where it is drawn takes it");
    } else
        check(false, "regions: at a pole, the aim still has a handle");
    set("region1.yaw", 0.0f);
    set("region1.pitch", 0.0f);
    kind(RegionKind::Spot);

    //  ---- the ping: a click that lands on a handle places nothing ----------------------------------
    if (spec.ping) {
        const auto before = spec.ping(*editor);
        if (const auto a = handle(K::Aim)) area->clickAt(view->pixelOf(*a));
        const auto after = spec.ping(*editor);
        check(before && after && dot(*before, *after) > 0.999999,
              "regions: a click on a handle leaves the ping where it was");
    }

    /*  ---- under a rate ------------------------------------------------------------------------------
        The region is drawn where the audio thread has it, turn included; a handle carried there writes
        the setting, the drawn angle less the turn. Writing the drawn angle adds the turn twice. */
    set("region1.yaw_rate", 45.0f);
    play(200);  // about half a second: a real 20-odd degrees
    frame();
    const auto drawn = view->regionsDrawnNow();
    check(!drawn.empty() && std::abs(drawn[0].region.yaw - live("region1.yaw") * kDeg2Rad) > 1e-3,
          "regions: a yaw rate turns the DRAWN region away from its set yaw");
    if (const auto a = handle(K::Aim)) area->dragBetween(view->pixelOf(*a), view->screenOf(Vec3{1.0, 0.0, 0.0}));
    frame();
    const auto landed = view->regionsDrawnNow();
    check(!landed.empty() && dot(aimOf(landed[0].region), Vec3{1.0, 0.0, 0.0}) > 0.999,
          "regions: an aim dragged to front lands at front, the turn subtracted and not added twice");

    /*  A held handle is re-applied every frame, not only on pointer moves: a region turning under a rate
        slips from under a still pointer otherwise. Catches: the re-application dropped from the frame. */
    if (const auto a = handle(K::Aim)) {
        const auto at = view->pixelOf(*a);
        area->pressAt(at);
        area->moveTo(at.translated(8.0f, 0.0f));
        const auto heldAt = at.translated(8.0f, 0.0f);
        for (int f = 0; f < 15; ++f)  // half a second of frames, pointer still
        {
            play(13);
            frame();
        }
        const auto now = handle(K::Aim);
        check(now && view->pixelOf(*now).getDistanceFrom(heldAt) < 2.0f,
              "regions: half a second later, pointer still and the rate running, the aim is still under it");
        area->releaseDrag();
    }
    set("region1.yaw_rate", 0.0f);

    spec.showRegion(*editor, false);
    frame();
    check(view->handles().empty(), "regions: with the region's page closed, no handles");

    /*  ---- what the engine plays, beside what is set --------------------------------------------------
        An LFO on the region's size: a thin live outline is drawn beside the set region, at a size the set
        one does not have; without the row, the region is drawn once. Catches: the live region not read from
        the scene, or drawn whether or not anything moves it. */
    {
        kind(RegionKind::Spot);
        set("region1.size", 40.0f);
        set("lfo1.sync", 0.0f);
        set("lfo1.rate", 2.0f);
        const auto sizeId = static_cast<ParamId>(m.byKey("region1.size"));
        const int channels = std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
        juce::AudioBuffer<float> buffer(channels, 128);
        juce::MidiBuffer midi;
        const auto play = [&](int blocks) {
            for (int i = 0; i < blocks; ++i) {
                buffer.clear();
                proc.processBlock(buffer, midi);  // the control step resolves the region and publishes it
                head.time += 128;
                if (i % 4 == 3) base.linkTick();
            }
            frame();
        };
        //  every live outline drawn, and those at a size the set region does not have
        const auto liveOnes = [&](bool moved) {
            int count = 0;
            for (const auto& r : view->regionsDrawnNow())
                if (r.live && (!moved || std::abs(r.region.size - 40.0 * kDeg2Rad) > 2.0 * kDeg2Rad)) ++count;
            return count;
        };
        play(200);  // a size just set glides in over 40 ms, and the outline shows it doing so: let it arrive
        check(liveOnes(false) == 0, "regions: a region nothing modulates is drawn once, with no live outline");
        base.document().edit(
            "size row", [sizeId](PluginState& st) { st.matrix.push_back({MatrixTab::Generators, 0, sizeId, 0.3}); });
        play(40);
        check(liveOnes(false) == 1 && liveOnes(true) == 1,
              "regions: an LFO on its size draws a live outline where the engine has it");
        base.document().edit("no size row", [](PluginState& st) { st.matrix.clear(); });
        play(200);  // size is smoothed over 40 ms: half a second is well past settled
        check(liveOnes(false) == 0, "regions: and without the row, the outline goes");
    }

    /*  ---- another instance's region, as its owner resolved it ------------------------------------
        A second instance on the bus sets a spot aimed left; its audio thread publishes it, and this
        window draws it as another's: never open, edge only. Catches: the record not published, not
        read, or drawn as this instance's own. */
    {
        Processor other;
        host::PluginProcessor& ob = other;
        other.prepareToPlay(48000.0, 128);
        ob.document().edit("region kind", [](PluginState& st) { st.regions[0].shape.kind = RegionKind::Spot; });
        if (auto* yaw = ob.hostParameter(static_cast<ParamId>(m.byKey("region1.yaw"))))
            yaw->setValueNotifyingHost(yaw->convertTo0to1(90.0f));
        const int channels = std::max(other.getTotalNumInputChannels(), other.getTotalNumOutputChannels());
        juce::AudioBuffer<float> buffer(channels, 128);
        juce::MidiBuffer midi;
        for (int i = 0; i < 30; ++i) {
            base.linkTick();
            ob.linkTick();
            buffer.clear();
            other.processBlock(buffer, midi);  // its control step resolves the region and publishes it
        }
        frame();
        int others = 0;
        bool left = false;
        for (const auto& r : view->regionsDrawnNow())
            if (!r.own) {
                ++others;
                left = left || (r.region.kind == RegionKind::Spot && dot(aimOf(r.region), Vec3{0.0, 1.0, 0.0}) > 0.99);
                check(!r.open, "regions: another instance's region is never open here");
            }
        check(others >= 1 && left, "regions: another instance's spot, aimed left, is drawn as another's");
        LinkBus::unlinkSession(base.identity().session);
    }
}

}  // namespace bambi::editor
