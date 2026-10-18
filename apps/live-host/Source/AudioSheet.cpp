// SPDX-License-Identifier: GPL-3.0-or-later
#include "AudioSheet.h"

#include <algorithm>
#include <cmath>

#include "bambi/ui/ChannelStrip.h"
#include "bambi/ui/Draw.h"
#include "bambi/ui/Widgets.h"

namespace bambi::livehost {

namespace {

namespace colour = ui::theme::colour;
namespace sh = ui::theme::sheet;
namespace ctl = ui::theme::controls;
using bambi::Output;

using ui::kSeparator;
const juce::String kDash = juce::String::fromUTF8("\xe2\x80\x93");

juce::String ms(int samples, double rate) { return juce::String(1000.0 * samples / rate, 1) + " ms"; }

//  A framed action as wide as its label needs, at `x`, centred on `cy`.
juce::Rectangle<float> button(juce::Graphics& g, float x, float cy, const juce::String& label) {
    const juce::Rectangle<float> box{x, cy - sh::control / 2.0f, ui::framedButtonWidth(label), sh::control};
    ui::drawFramedButton(g, box, label, true);
    return box;
}

}  // namespace

void AudioSheet::show(Shown shown) {
    s_ = std::move(shown);
    if (std::none_of(s_.inputs.begin(), s_.inputs.end(), [&](const live::HostInput& i) { return i.id == input_; }))
        input_ = s_.inputs.empty() ? 0 : s_.inputs.front().id;
    repaint();
}

void AudioSheet::setLoad(float load) {
    s_.load = load;
    if (isOpen() && tab_ == Tab::Interface) repaint();
}

void AudioSheet::setLead(double ms, double spreadMs, int count) {
    s_.leadMs = ms;
    s_.leadSpreadMs = spreadMs;
    s_.leadCount = count;
    if (isOpen() && tab_ == Tab::Midi) repaint();
}

void AudioSheet::dismiss() {
    if (arming_ || menu_) {
        arming_.reset();
        menu_ = false;
        repaint();
        return;
    }
    close();
}

void AudioSheet::edit(const std::function<void(live::AudioSettings&)>& how) {
    live::AudioSettings next = s_.settings;
    how(next);
    s_.settings = next;
    if (change) change(next);
    repaint();
}

live::Row AudioSheet::outputRow() const { return live::rowOf(live::aggregateOf(s_.settings.devices, s_.system)); }
live::Row AudioSheet::inputRow() const {
    return live::rowOf(live::aggregateOf(s_.settings.devices, s_.system), true);
}

juce::String AudioSheet::where(const live::Row& row, int at, int width) const {
    const auto first = row.channel(at), last = row.channel(at + width - 1);
    if (first.none()) return "nowhere";
    const auto nameOn = [&](const live::DeviceChannel& c) {
        for (const live::AudioDevice& d : row.devices)
            if (d.uid == c.device) return juce::String(d.name);
        return juce::String();
    };
    if (width <= 1) return nameOn(first) + " " + juce::String(first.number);
    if (last.none() || last.device != first.device)
        return nameOn(first) + " " + juce::String(first.number) + " + " +
               (last.none() ? juce::String("past the last") : nameOn(last) + " " + juce::String(last.number));
    return nameOn(first) + " " + juce::String(first.number) + kDash + juce::String(last.number);
}

juce::Rectangle<float> AudioSheet::box() const {
    const auto all = getLocalBounds().toFloat();
    const float w = std::min(sh::width, all.getWidth() - 2.0f * sh::inset);
    const float h = std::min(sh::height, all.getHeight() - 2.0f * sh::inset);
    return juce::Rectangle<float>(w, h).withCentre(all.getCentre());
}

void AudioSheet::paint(juce::Graphics& g) {
    const auto b = box();
    paintSheet(g, b);
    auto inner = b.reduced(sh::pad);

    //  the head: the title, the tabs with what each holds, the cross
    auto head = inner.removeFromTop(sh::head);
    const auto titleFont = ui::font(ui::theme::type::body, ui::theme::type::trackingHeading);
    ui::text(g, "audio", titleFont, colour::text, head, juce::Justification::centredLeft);
    const float tabHeight = ctl::tabBarHeight - ctl::matrixTabsTop;
    const auto row = outputRow();
    const std::vector<juce::String> names{
        "interface " + juce::String(static_cast<int>(live::aggregateOf(s_.settings.devices, s_.system).size())),
        "outputs " + juce::String(row.width()), "inputs " + juce::String(static_cast<int>(s_.inputs.size())),
        "midi " + juce::String(static_cast<int>(s_.midiInputs.size()))};
    paintTabRowAt(g, head.getX() + ui::textWidth(titleFont, "audio") + sh::gap + ctl::tabPadX,
                  head.getCentreY() - tabHeight / 2.0f, tabHeight, names, static_cast<int>(tab_), [this](int t) {
                      tab_ = static_cast<Tab>(t);
                      arming_.reset();
                      menu_ = false;
                      repaint();
                  });
    const juce::Rectangle<float> cross{head.getRight() - ctl::closeGlyph, head.getCentreY() - ctl::closeGlyph / 2.0f,
                                       ctl::closeGlyph, ctl::closeGlyph};
    addClickArea(ui::drawCloseCross(g, cross), [this] { close(); });
    ui::ruleH(g, b.getX(), b.getRight(), head.getBottom(), colour::ruleStrong);

    auto page = inner.withTrimmedTop(sh::gap);
    //  nothing past the frame: what does not fit is cut, not drawn over the window
    juce::Graphics::ScopedSaveState clip(g);
    g.reduceClipRegion(page.toNearestInt());
    if (s_.problem.isNotEmpty()) {
        const auto line = page.removeFromBottom(sh::line);
        ui::text(g, s_.problem, ui::labelFont(), colour::text, line, juce::Justification::centredLeft);
    }
    if (tab_ == Tab::Interface)
        interfacePage(g, page);
    else if (tab_ == Tab::Outputs)
        outputsPage(g, page);
    else if (tab_ == Tab::Inputs)
        inputsPage(g, page);
    else
        midiPage(g, page);
}

// ---- interface ----------------------------------------------------------------------------------

float AudioSheet::interfacePage(juce::Graphics& g, juce::Rectangle<float> page) {
    float y = ui::drawGroupHeading(g, page.getX(), page.getY(), page.getWidth(), "devices");
    const auto& chosen = s_.settings.devices;
    const auto joined = live::aggregateOf(chosen, s_.system);
    for (std::size_t k = 0; k < chosen.size(); ++k) {
        const std::string uid = chosen[k];
        const auto d = std::find_if(joined.begin(), joined.end(), [&](const live::AudioDevice& x) { return x.uid == uid; });
        const juce::Rectangle<float> line{page.getX(), y, page.getWidth(), sh::line};
        auto rest = line;
        const bool here = d != joined.end();
        ui::text(g, here ? juce::String(d->name) : juce::String(uid), ui::labelFont(),
                 here ? colour::text : colour::inactive, rest.removeFromLeft(sh::nameColumn),
                 juce::Justification::centredLeft);
        if (here) {
            ui::text(g, juce::String(d->outs) + " out" + kSeparator + juce::String(d->ins) + " in", ui::smallFont(),
                     colour::label, rest.removeFromLeft(sh::ioColumn), juce::Justification::centredLeft);
            ui::text(g, k == 0 ? "the clock" : "drift corrected", ui::smallFont(), colour::label,
                     rest.removeFromLeft(sh::roleColumn), juce::Justification::centredLeft);
            ui::text(g, ms(d->outLatency, s_.rate), ui::smallFont(), colour::label,
                     rest.removeFromLeft(sh::latencyColumn), juce::Justification::centredLeft);
        } else {
            ui::text(g, "not here, or past what the row holds", ui::smallFont(), colour::inactive,
                     rest.removeFromLeft(sh::ioColumn + sh::roleColumn + sh::latencyColumn), juce::Justification::centredLeft);
        }
        const juce::Rectangle<float> x{line.getRight() - ctl::closeGlyph, line.getCentreY() - ctl::closeGlyph / 2.0f,
                                       ctl::closeGlyph, ctl::closeGlyph};
        addClickArea(ui::drawCloseCross(g, x), [this, k] {
            edit([k](live::AudioSettings& s) { s.devices.erase(s.devices.begin() + static_cast<long>(k)); });
        });
        if (k > 0 && here) {
            const auto make = button(g, x.getX() - sh::gap - ui::framedButtonWidth("make clock"), line.getCentreY(),
                                     "make clock");
            addClickArea(make, [this, k] {
                edit([k](live::AudioSettings& s) {
                    std::rotate(s.devices.begin(), s.devices.begin() + static_cast<long>(k),
                                s.devices.begin() + static_cast<long>(k) + 1);
                });
            });
        }
        y += sh::line;
    }
    const auto add = button(g, page.getX(), y + sh::control / 2.0f + ctl::segmentGap, "+ device");
    addClickArea(add, [this] {
        menu_ = !menu_;
        repaint();
    });
    y = add.getBottom() + ctl::segmentGap;
    if (joined.size() > 1) {
        int least = joined.front().outLatency, most = least;
        for (const auto& d : joined) {
            least = std::min(least, d.outLatency);
            most = std::max(most, d.outLatency);
        }
        ui::text(g,
                 "The host holds the quicker devices back by up to " + ms(most - least, s_.rate) +
                     ", so every device plays in step; one that is not the clock is resampled to follow it.",
                 ui::labelFont(), colour::label, {page.getX(), y, page.getWidth(), sh::line},
                 juce::Justification::centredLeft);
        y += sh::line;
    }

    y = ui::drawGroupHeading(g, page.getX(), y + sh::gap, page.getWidth(), "timing");
    ui::drawFactRow(g, {page.getX(), y, page.getWidth(), sh::line}, "rate",
                    juce::String(s_.rate / 1000.0, juce::roundToInt(s_.rate) % 1000 == 0 ? 0 : 1) +
                        " kHz" + kSeparator + "Live's");
    y += sh::line + ctl::segmentGap;
    ui::text(g, "buffer", ui::labelFont(), colour::label, {page.getX(), y, sh::labelColumn, sh::control},
             juce::Justification::centredLeft);
    const std::vector<juce::String> buffers{"32", "64", "128", "256"};
    const int chosenBuffer = static_cast<int>(
        std::find(buffers.begin(), buffers.end(), juce::String(s_.settings.buffer)) - buffers.begin());
    const auto cells = ui::drawSegments(
        g, {page.getX() + sh::labelColumn, y, ui::segmentsWidth(buffers), sh::control}, buffers, chosenBuffer, true);
    for (std::size_t i = 0; i < cells.size(); ++i)
        addClickArea(cells[i], [this, v = buffers[i].getIntValue()] {
            edit([v](live::AudioSettings& s) { s.buffer = v; });
        });
    y += sh::control + sh::gap;

    y = ui::drawGroupHeading(g, page.getX(), y, page.getWidth(), "order");
    //  every plugin's, with the set: a new one loads every plugin again
    ui::text(g, "every plugin", ui::labelFont(), colour::label, {page.getX(), y, sh::labelColumn, sh::control},
             juce::Justification::centredLeft);
    const std::vector<juce::String> orders{"1", "2", "3", "4", "5", "6", "7"};
    const auto orderCells = ui::drawSegments(
        g, {page.getX() + sh::labelColumn, y, ui::segmentsWidth(orders), sh::control}, orders, s_.order - 1, true);
    for (std::size_t i = 0; i < orderCells.size(); ++i)
        addClickArea(orderCells[i], [this, order = static_cast<int>(i) + 1] {
            if (order != s_.order && changeOrder) changeOrder(order);
        });
    const int field = (s_.order + 1) * (s_.order + 1);
    ui::text(g, juce::String(field) + " channels" + kSeparator + "with the set", ui::smallFont(), colour::label,
             {page.getX() + sh::labelColumn + ui::segmentsWidth(orders) + sh::gap, y, sh::valueColumn * 2.0f,
              sh::control},
             juce::Justification::centredLeft);
    y += sh::control + ctl::segmentGap;
    ui::text(g, "load" + kSeparator + "this machine, now", ui::labelFont(), colour::label,
             {page.getX(), y, sh::wideLabel, sh::line}, juce::Justification::centredLeft);
    const float share = std::clamp(s_.load, 0.0f, 1.0f);
    ui::BarContent bar;
    bar.fillTo = share;
    bar.ink = colour::now;
    ui::drawValueBar(
        g, {page.getX() + sh::wideLabel, y + (sh::line - ctl::barHeight) / 2.0f, sh::barWidth, ctl::barHeight}, bar);
    ui::text(g, juce::String(juce::roundToInt(100.0f * share)) + "% of each block", ui::smallFont(), colour::label,
             {page.getX() + sh::wideLabel + sh::barWidth + sh::gap, y, sh::valueColumn, sh::line},
             juce::Justification::centredLeft);
    y += sh::line;

    //  the menu last, over what is under it; a press anywhere else closes it
    if (menu_) {
        std::vector<live::AudioDevice> offered;
        std::vector<ui::MenuItem> items;
        for (const live::AudioDevice& d : s_.system) {
            if (std::find(chosen.begin(), chosen.end(), d.uid) != chosen.end()) continue;
            offered.push_back(d);
            items.push_back({juce::String(d.name),
                             d.wireless ? juce::String("wireless: not offered")
                                        : juce::String(d.outs) + " out" + kSeparator + juce::String(d.ins) + " in",
                             !d.wireless});
        }
        addClickArea(getLocalBounds().toFloat(), [this] {
            menu_ = false;
            repaint();
        });
        const auto menu = ui::drawMenu(g, {add.getX(), add.getBottom()}, sh::menuWidth, items,
                                       "Live plays on bambi's loop device; the host joins it to these.");
        addClickArea(menu.box, [] {});
        for (std::size_t i = 0; i < menu.items.size(); ++i)
            if (items[i].offered)
                addClickArea(menu.items[i], [this, uid = offered[i].uid] {
                    menu_ = false;
                    edit([uid](live::AudioSettings& s) { s.devices.push_back(uid); });
                });
    }
    return y;
}

// ---- outputs ------------------------------------------------------------------------------------

juce::String AudioSheet::nameOf(const live::Out& out) const {
    if (out.main()) return "Live's Main";
    const juce::String which = out.which == Output::Headphones ? "headphones" : "speakers";
    return s_.decoders.size() > 1 ? decoderName(out.decoder) + " " + which : which;
}

juce::String AudioSheet::decoderName(std::uint64_t id) const {
    for (std::size_t k = 0; k < s_.decoders.size(); ++k)
        if (s_.decoders[k].id == id) return "decoder " + juce::String(static_cast<int>(k) + 1);
    return "a decoder";
}

void AudioSheet::outputRow(juce::Graphics& g, juce::Rectangle<float> line, const live::Out& out) {
    const auto row = outputRow();
    if (out == chosen_) {
        g.setColour(colour::highlight);
        g.fillRect(line);
    }
    addClickArea(line, [this, out] {
        chosen_ = out;
        repaint();
    });
    float x = line.getX() + ctl::segmentGap;
    const auto bus = live::busChannels(out, s_.decoders);
    const int width = bus.empty() ? 0 : bus.back() - bus.front() + 1;
    juce::String what = out.main() ? juce::String("Live's Main") : out.which == Output::Headphones ? "headphones" : "speakers";
    ui::text(g, what, ui::labelFont(), colour::text, {x, line.getY(), sh::outputName, line.getHeight()},
             juce::Justification::centredLeft);
    x += sh::outputName;
    //  how wide it is, and for the speakers the Decoder's layout: what is placed, never set here
    juce::String size = juce::String(width) + " channels";
    if (!out.main() && out.which == Output::Speakers)
        for (const auto& d : s_.decoders)
            if (d.id == out.decoder && !d.report.layoutName.empty()) size += kSeparator + juce::String(d.report.layoutName);
    ui::text(g, size, ui::smallFont(), colour::label, {x, line.getY(), sh::outputWhere, line.getHeight()},
             juce::Justification::centredLeft);
    x += sh::outputWhere;
    const auto taken = live::takenBy(out, s_.settings.at(out), row, s_.decoders);
    const juce::String at = taken.empty() ? juce::String("placed nowhere: not heard")
                                          : where(row, taken.front(), taken.back() - taken.front() + 1);
    ui::text(g, at, ui::smallFont(), taken.empty() ? colour::text : colour::label,
             {x, line.getY(), sh::outputWhere, line.getHeight()}, juce::Justification::centredLeft);
    x += sh::outputWhere;

    //  its copies, the host's, then the ways to make another
    const float chipTop = line.getCentreY() - sh::control / 2.0f;
    for (std::size_t i = 0; i < s_.settings.copies.size(); ++i) {
        const live::Copy& c = s_.settings.copies[i];
        if (!(c.of == out)) continue;
        const auto copied = live::takenBy(c.of, c.at, row, s_.decoders);
        const juce::String label =
            c.toLive ? juce::String("also back to Live")
                     : "also " + (copied.empty() ? juce::String("nowhere")
                                                 : where(row, copied.front(), copied.back() - copied.front() + 1));
        const auto chip = ui::drawChip(g, {x, chipTop}, sh::control, label, colour::text, std::nullopt, true);
        addClickArea(chip.cross, [this, i] {
            edit([i](live::AudioSettings& s) { s.copies.erase(s.copies.begin() + static_cast<long>(i)); });
        });
        x = chip.box.getRight() + ctl::segmentGap;
    }
    if (arming_ && *arming_ == out) {
        const juce::String waiting = "click the map" + kSeparator + "esc cancels";
        const juce::Rectangle<float> box{x, chipTop, ui::framedButtonWidth(waiting), sh::control};
        ui::drawWaiting(g, box, waiting, ui::smallFont());
        addClickArea(box, [this] {
            arming_.reset();
            repaint();
        });
        return;
    }
    const auto copy = ui::drawChip(g, {x, chipTop}, sh::control, "+ copy", colour::label);
    addClickArea(copy.box, [this, out] {
        arming_ = out;
        chosen_ = out;
        repaint();
    });
    x = copy.box.getRight() + ctl::segmentGap;
    const bool backAlready = std::any_of(s_.settings.copies.begin(), s_.settings.copies.end(),
                                         [&](const live::Copy& c) { return c.of == out && c.toLive; });
    if (!out.main() && bus.size() == 2 && !backAlready) {
        const auto back = ui::drawChip(g, {x, chipTop}, sh::control, "+ back to Live", colour::label);
        addClickArea(back.box, [this, out] {
            edit([out](live::AudioSettings& s) { s.copies.push_back({out, {}, true}); });
        });
    }
}

float AudioSheet::outputsPage(juce::Graphics& g, juce::Rectangle<float> page) {
    const auto row = outputRow();
    float y = page.getY();
    outputRow(g, {page.getX(), y, page.getWidth(), sh::line}, live::kLiveMain);
    y += sh::line + ctl::segmentGap;

    //  each Decoder: what it plays, to be placed; how it plays is set on its own page
    for (const live::PlayingDecoder& d : s_.decoders) {
        const juce::Rectangle<float> head{page.getX(), y, page.getWidth(), sh::noteLine};
        ui::text(g, decoderName(d.id) + kSeparator + "what it plays, and how, is set on its page", ui::smallFont(),
                 colour::label, head, juce::Justification::centredLeft);
        const auto open = ui::drawChip(g, {head.getRight() - ui::chipWidth("open", sh::control, false, false), y},
                                       sh::control, "open", colour::label);
        addClickArea(open.box, [this, id = d.id] {
            if (openDecoder) openDecoder(id);
        });
        y += std::max(sh::noteLine, sh::control);
        bool any = false;
        for (const Output which : {Output::Headphones, Output::Speakers}) {
            const live::Out out{d.id, which};
            if (live::busChannels(out, s_.decoders).empty()) continue;
            outputRow(g, {page.getX(), y, page.getWidth(), sh::line}, out);
            y += sh::line;
            any = true;
        }
        if (!any) {
            ui::text(g, "it plays nothing", ui::smallFont(), colour::label,
                     {page.getX() + ctl::segmentGap, y, page.getWidth(), sh::noteLine}, juce::Justification::centredLeft);
            y += sh::noteLine;
        }
        y += ctl::segmentGap;
    }
    ui::text(g,
             "A copy is the host's, after the decoder: the same signal on other channels, or a pair back to Live's "
             "bambi track for recording. Live renders nothing that does not come back to it.",
             ui::smallFont(), colour::label, {page.getX(), y, page.getWidth(), sh::noteLine},
             juce::Justification::centredLeft);
    y += sh::noteLine + sh::gap;

    //  the map, at the page's foot
    if (row.width() == 0) {
        ui::text(g, "No device chosen: the headphones go back to Live, on the bambi track. Choose one on interface.",
                 ui::labelFont(), colour::label, {page.getX(), page.getBottom() - sh::line, page.getWidth(), sh::line},
                 juce::Justification::centredLeft);
        return y;
    }
    std::vector<Block> blocks;
    const auto labelsOf = [&](const live::Out& out, const std::vector<int>& taken) {
        std::vector<juce::String> labels;
        const auto bus = live::busChannels(out, s_.decoders);
        if (bus.size() == 2) return std::vector<juce::String>{"L", "R"};
        for (std::size_t i = 0; i < taken.size() && i < bus.size(); ++i)
            labels.push_back(juce::String(bus[i] - bus.front() + 1));
        return labels;
    };
    const auto place = [&](const live::Out& out, const live::DeviceChannel& at, bool host) {
        const auto taken = live::takenBy(out, at, row, s_.decoders);
        if (!taken.empty()) blocks.push_back({taken, labelsOf(out, taken), host, !host && out == chosen_});
    };
    place(live::kLiveMain, s_.settings.main, true);
    if (!blocks.empty()) blocks.back().chosen = chosen_.main();
    for (const live::PlayingDecoder& d : s_.decoders)
        for (const Output which : {Output::Headphones, Output::Speakers})
            place({d.id, which}, s_.settings.at({d.id, which}), false);
    for (const live::Copy& c : s_.settings.copies)
        if (!c.toLive) place(c.of, c.at, true);
    const juce::String what = arming_ ? "a click puts a copy of " + nameOf(*arming_) + " there"
                                      : "the outputs of every device" + kSeparator + "a click puts " + nameOf(chosen_) +
                                            " there";
    const float mapHeight = sh::line + sh::noteLine + ui::theme::channelStrip::cell + ui::theme::channelStrip::barGap +
                            ui::theme::channelStrip::bar;
    const float top = std::max(y, page.getBottom() - mapHeight);
    ui::text(g, what, ui::labelFont(), colour::label, {page.getX(), top, page.getWidth(), sh::line},
             juce::Justification::centredLeft);
    return map(g, page.getX(), top + sh::line, page.getWidth(), row, blocks, outputsScroll_, [this, row](int c) {
        if (arming_) {
            const live::Out of = *arming_;
            arming_.reset();
            edit([&](live::AudioSettings& s) { s.copies.push_back({of, row.channel(c), false}); });
            return;
        }
        const live::Out out = chosen_;
        edit([&](live::AudioSettings& s) {
            if (out.main()) {
                s.main = row.channel(c);
                return;
            }
            const auto had = std::find_if(s.placed.begin(), s.placed.end(), [&](const live::Placed& x) { return x.out == out; });
            if (had != s.placed.end())
                had->at = row.channel(c);
            else
                s.placed.push_back({out, row.channel(c)});
        });
    });
}

// ---- the map ------------------------------------------------------------------------------------

float AudioSheet::map(juce::Graphics& g, float x, float top, float width, const live::Row& row,
                      const std::vector<Block>& blocks, float& scroll, std::function<void(int)> place) {
    std::vector<int> groups;
    for (const live::AudioDevice& d : row.devices)
        if (const int count = row.inputs ? d.ins : d.outs; count > 0) groups.push_back(count);
    const float labelLine = sh::noteLine;
    //  the first channel an output takes is brought into view, so the map opens on what is placed
    int reveal = 0;
    for (const Block& b : blocks)
        if (b.chosen && !b.channels.empty()) reveal = b.channels.front();
    const int revealNow = reveal != revealed_ ? reveal : 0;
    revealed_ = reveal;
    const auto strip = ui::ChannelStrip::layout(x, top + labelLine, width, groups, scroll, revealNow);
    //  under the cells, so a press finds a cell first
    addRegion({strip.viewport.withTop(top), {}, {}, {}, {}, {}, [shift = &scroll](float by) { *shift += by; }});
    g.saveState();
    g.reduceClipRegion(strip.viewport.withTop(top).toNearestIntEdges());
    int c = 1;
    std::size_t group = 0;
    for (const live::AudioDevice& d : row.devices) {
        const int count = row.inputs ? d.ins : d.outs;
        if (count == 0) continue;
        const float left = strip.groupLeft(group++);
        const auto& last = strip.cells[static_cast<std::size_t>(c - 1 + count - 1)];
        ui::text(g, juce::String(d.name), ui::smallFont(), colour::label, {left, top, last.getRight() - left, labelLine},
                 juce::Justification::centredLeft);
        for (int i = 0; i < count; ++i, ++c) {
            const juce::Rectangle<float> box = strip.cells[static_cast<std::size_t>(c - 1)];
            std::vector<std::pair<const Block*, std::size_t>> here;
            for (const Block& b : blocks)
                for (std::size_t k = 0; k < b.channels.size(); ++k)
                    if (b.channels[k] == c) here.push_back({&b, k});
            const bool clash = here.size() > 1;
            const Block* top1 = here.empty() ? nullptr : here.front().first;
            const bool chosen = top1 != nullptr && top1->chosen && !clash;
            if (chosen) {
                g.setColour(colour::chosen);
                g.fillRect(box);
            }
            const juce::Colour frame = clash             ? colour::ruleStrong
                                       : top1 == nullptr ? colour::rule
                                       : top1->host      ? colour::label
                                       : chosen          ? colour::chosen
                                                         : top1->ink;
            ui::drawFrame(g, box, frame, ui::theme::stroke::rule, clash || (top1 != nullptr && top1->host));
            const juce::String label = clash             ? juce::String::fromUTF8("\xc3\x97") + juce::String(here.size())
                                       : top1 == nullptr ? juce::String(i + 1)
                                       : here.front().second < top1->labels.size()
                                           ? top1->labels[here.front().second]
                                           : juce::String();
            ui::text(g, label, ui::smallFont(),
                     chosen ? colour::textInverse : top1 == nullptr ? colour::inactive : colour::text, box,
                     juce::Justification::centred);
            addRegion({box.getIntersection(strip.viewport), {}, {}, {}, [place, c] { place(c); }, {}, {}});
        }
    }
    g.restoreState();
    strip.paintBar(g, colour::label, scroll);
    return strip.bottom;
}

// ---- inputs -------------------------------------------------------------------------------------

float AudioSheet::inputsPage(juce::Graphics& g, juce::Rectangle<float> page) {
    const auto row = inputRow();
    float y = page.getY();
    ui::text(g,
             "An input of any device, a source of the field at the least latency: an Encoder, inserts and sends as any "
             "track's, beside Live's.",
             ui::labelFont(), colour::label, {page.getX(), y, page.getWidth(), sh::line},
             juce::Justification::centredLeft);
    y += sh::line + ctl::segmentGap;
    if (row.width() == 0) {
        ui::text(g, "None of the chosen devices has inputs.", ui::labelFont(), colour::label,
                 {page.getX(), y, page.getWidth(), sh::line}, juce::Justification::centredLeft);
        return y + sh::line;
    }
    const auto atOf = [&](live::TrackId id) {
        for (const auto& in : s_.settings.inputs)
            if (in.input == id) return row.at(in.at);
        return 0;
    };
    const auto changeAll = [this](std::vector<live::HostInput> inputs, live::AudioSettings settings) {
        s_.inputs = inputs;
        s_.settings = settings;
        if (changeInputs) changeInputs(std::move(inputs), std::move(settings));
        repaint();
    };
    for (const live::HostInput& in : s_.inputs) {
        const juce::Rectangle<float> line{page.getX(), y, page.getWidth(), sh::line};
        if (in.id == input_) {
            g.setColour(colour::highlight);
            g.fillRect(line);
        }
        addClickArea(line, [this, id = in.id] {
            input_ = id;
            repaint();
        });
        ui::text(g, juce::String(in.name), ui::labelFont(), colour::text,
                 {line.getX() + ctl::segmentGap, y, sh::inputName, sh::line}, juce::Justification::centredLeft);
        const int at = atOf(in.id);
        const bool oneDevice = at != 0 && row.deviceOf(at) == row.deviceOf(at + in.width - 1);
        ui::text(g, at == 0 ? juce::String("nowhere") : where(row, at, in.width), ui::smallFont(),
                 oneDevice || at == 0 ? colour::label : colour::text,
                 {line.getX() + sh::inputName, y, sh::inputWhere, sh::line}, juce::Justification::centredLeft);
        if (at != 0 && !oneDevice)
            ui::text(g, "not on one device: not heard", ui::smallFont(), colour::text,
                     {line.getX() + sh::inputName + sh::inputWhere, y, sh::nameColumn, sh::line},
                     juce::Justification::centredLeft);
        const std::vector<juce::String> widths{"mono", "stereo"};
        const auto cells =
            ui::drawSegments(g,
                             {line.getRight() - 2.0f * sh::gap - ctl::closeGlyph - ui::segmentsWidth(widths),
                              line.getCentreY() - sh::control / 2.0f, ui::segmentsWidth(widths), sh::control},
                             widths, in.width - 1, true);
        for (std::size_t i = 0; i < cells.size(); ++i)
            addClickArea(cells[i], [this, changeAll, id = in.id, width = static_cast<int>(i) + 1] {
                auto inputs = s_.inputs;
                for (auto& x : inputs)
                    if (x.id == id) x.width = width;
                changeAll(inputs, s_.settings);
            });
        const juce::Rectangle<float> cross{line.getRight() - ctl::closeGlyph - ctl::segmentGap,
                                           line.getCentreY() - ctl::closeGlyph / 2.0f, ctl::closeGlyph,
                                           ctl::closeGlyph};
        addClickArea(ui::drawCloseCross(g, cross), [this, changeAll, id = in.id] {
            auto inputs = s_.inputs;
            auto settings = s_.settings;
            inputs.erase(std::remove_if(inputs.begin(), inputs.end(), [&](const auto& x) { return x.id == id; }),
                         inputs.end());
            settings.inputs.erase(std::remove_if(settings.inputs.begin(), settings.inputs.end(),
                                                 [&](const auto& x) { return x.input == id; }),
                                  settings.inputs.end());
            changeAll(inputs, settings);
        });
        y += sh::line;
    }
    const auto add = button(g, page.getX(), y + sh::control / 2.0f + ctl::segmentGap, "+ input");
    addClickArea(add, [this, changeAll, row] {
        auto inputs = s_.inputs;
        auto settings = s_.settings;
        live::TrackId id = live::kFirstInput;
        for (const auto& x : inputs) id = std::max(id, x.id + 1);
        //  the first channel no input takes yet
        int free = 1;
        while (free < row.width() && std::any_of(settings.inputs.begin(), settings.inputs.end(),
                                                 [&](const auto& x) { return row.at(x.at) == free; }))
            ++free;
        inputs.push_back({id, "input " + std::to_string(inputs.size() + 1), 1});
        settings.inputs.push_back({id, row.channel(free)});
        input_ = id;
        changeAll(inputs, settings);
    });
    y = add.getBottom() + sh::gap;

    const auto chosen = std::find_if(s_.inputs.begin(), s_.inputs.end(),
                                     [&](const live::HostInput& i) { return i.id == input_; });
    if (chosen == s_.inputs.end()) return y;
    std::vector<Block> blocks;
    for (const live::HostInput& in : s_.inputs) {
        const int at = atOf(in.id);
        if (at == 0) continue;
        Block b;
        for (int k = 0; k < in.width; ++k) b.channels.push_back(at + k);
        b.labels = in.width == 2 ? std::vector<juce::String>{"L", "R"}
                                 : std::vector<juce::String>{juce::String(in.name).substring(0, 3)};
        b.chosen = in.id == input_;
        b.ink = colour::in;
        blocks.push_back(std::move(b));
    }
    const float mapHeight = sh::line + sh::noteLine + ui::theme::channelStrip::cell + ui::theme::channelStrip::barGap +
                            ui::theme::channelStrip::bar;
    const float top = std::max(y, page.getBottom() - mapHeight);
    ui::text(g, "the inputs of every device" + kSeparator + "a click puts " + juce::String(chosen->name) + " there",
             ui::labelFont(), colour::label, {page.getX(), top, page.getWidth(), sh::line},
             juce::Justification::centredLeft);
    return map(g, page.getX(), top + sh::line, page.getWidth(), row, blocks, inputsScroll_, [this, changeAll, row](int c) {
        auto settings = s_.settings;
        bool found = false;
        for (auto& x : settings.inputs)
            if (x.input == input_) {
                x.at = row.channel(c);
                found = true;
            }
        if (!found) settings.inputs.push_back({input_, row.channel(c)});
        changeAll(s_.inputs, settings);
    });
}

// ---- midi ---------------------------------------------------------------------------------------

float AudioSheet::midiPage(juce::Graphics& g, juce::Rectangle<float> page) {
    float y = ui::drawGroupHeading(g, page.getX(), page.getY(), page.getWidth(), "inputs");
    for (std::size_t k = 0; k < s_.midiInputs.size(); ++k) {
        auto line = juce::Rectangle<float>{page.getX(), y, page.getWidth(), sh::line};
        ui::text(g, s_.midiInputs[k], ui::labelFont(), colour::text, line.removeFromLeft(sh::nameColumn),
                 juce::Justification::centredLeft);
        if (k + 1 == s_.midiInputs.size())
            ui::text(g, "the host's own: Live's MIDI tracks send here", ui::smallFont(), colour::label, line,
                     juce::Justification::centredLeft);
        y += sh::line;
    }
    if (s_.midiInputs.empty()) {
        ui::text(g, "none", ui::labelFont(), colour::inactive, {page.getX(), y, page.getWidth(), sh::line},
                 juce::Justification::centredLeft);
        y += sh::line;
    }
    ui::text(g, "Every plugin hears all of it, and answers to the channel and note it learned.", ui::smallFont(),
             colour::label, {page.getX(), y, page.getWidth(), sh::line}, juce::Justification::centredLeft);
    y += sh::line;

    y = ui::drawGroupHeading(g, page.getX(), y + sh::gap, page.getWidth(), "Live's MIDI");
    const double held = s_.settings.liveMidiMs;
    ui::text(g, "held back", ui::labelFont(), colour::label, {page.getX(), y, sh::labelColumn, sh::control},
             juce::Justification::centredLeft);
    float x = page.getX() + sh::labelColumn;
    const auto step = [this](double by) {
        edit([by](live::AudioSettings& a) { a.liveMidiMs = std::clamp(std::round(a.liveMidiMs + by), 0.0, live::kMaxLiveMidiMs); });
    };
    const auto less = button(g, x, y + sh::control / 2.0f, kDash);
    addClickArea(less, [step] { step(-1.0); });
    x = less.getRight() + ctl::segmentGap;
    const juce::String value = juce::String(held, 1) + " ms";
    ui::text(g, value, ui::labelFont(), colour::text, {x, y, sh::valueColumn, sh::control},
             juce::Justification::centred);
    x += sh::valueColumn + ctl::segmentGap;
    const auto more = button(g, x, y + sh::control / 2.0f, "+");
    addClickArea(more, [step] { step(1.0); });
    x = more.getRight() + sh::gap;
    const juce::Rectangle<float> measureAt{x, y, ui::framedButtonWidth("measure"), sh::control};
    ui::drawFramedButton(g, measureAt, "measure", true, s_.measuring);
    addClickArea(measureAt, [this] {
        s_.measuring = !s_.measuring;
        if (measure) measure(s_.measuring);
        repaint();
    });
    y += sh::control + ctl::segmentGap;

    if (s_.leadCount > 0) {
        auto line = juce::Rectangle<float>{page.getX(), y, page.getWidth(), sh::line};
        ui::text(g,
                 "measured " + juce::String(s_.leadMs, 1) + " ms " + juce::String::fromUTF8("\xc2\xb1") + " " +
                     juce::String(s_.leadSpreadMs, 1) + kSeparator + juce::String(s_.leadCount) +
                     (s_.leadCount == 1 ? " note" : " notes"),
                 ui::labelFont(), colour::text, line.removeFromLeft(sh::nameColumn + sh::ioColumn),
                 juce::Justification::centredLeft);
        const double found = std::clamp(s_.leadMs, 0.0, live::kMaxLiveMidiMs);
        const auto use = button(g, line.getX(), line.getCentreY(), "use");
        addClickArea(use, [this, found] { edit([found](live::AudioSettings& a) { a.liveMidiMs = found; }); });
        y += sh::line;
    }
    ui::text(g,
             s_.measuring ? "Listening: a MIDI track sending to bambi, and on Main a click on its notes, nothing else."
                          : "Measure with a MIDI track sending to bambi and, on Main, a click on its notes.",
             ui::smallFont(), colour::label, {page.getX(), y, page.getWidth(), sh::line},
             juce::Justification::centredLeft);
    y += sh::line;
    return y;
}

}  // namespace bambi::livehost
