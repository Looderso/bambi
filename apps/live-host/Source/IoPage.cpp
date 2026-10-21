// SPDX-License-Identifier: GPL-3.0-or-later
#include "IoPage.h"

#include <algorithm>
#include <cmath>

#include "bambi/ui/ChannelStrip.h"
#include "bambi/ui/Draw.h"
#include "bambi/ui/Tiles.h"
#include "bambi/ui/Widgets.h"

namespace bambi::livehost {

namespace {

namespace colour = ui::theme::colour;
namespace sh = ui::theme::sheet;
namespace ctl = ui::theme::controls;
namespace cs = ui::theme::channelStrip;
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

//  The `index`th tile of a grid of `columns` across `area`, from `top`: its frame inside `area`, as a page's are.
juce::Rectangle<float> gridCell(juce::Rectangle<float> area, float top, int index, int columns) {
    return ui::tileCell(area.reduced(ctl::tileBoxPadX, 0.0f), top + ctl::tileBoxPadY, index, ui::theme::type::value,
                        columns);
}

//  Where that grid of `count` tiles ends, its frames included.
float gridBottom(float top, int count, int columns) {
    return ui::tilesBottom(top + ctl::tileBoxPadY, count, ui::theme::type::value, columns) + ctl::tileBoxPadY;
}

//  A thing in a tab's list: its frame, and while it is what a click on a channel places, the selected row's marks.
void itemFrame(juce::Graphics& g, juce::Rectangle<float> box, bool chosen) {
    if (chosen) ui::drawSelectedRow(g, box);
    ui::drawFrame(g, box, colour::rule, ui::theme::stroke::rule, false);
}

//  A thing's colour before its name: filled in its owner's colour, or dashed in it for the host's own copy.
void swatch(juce::Graphics& g, float x, float cy, juce::Colour ink, bool dashed) {
    const juce::Rectangle<float> box{x, cy - sh::swatch / 2.0f, sh::swatch, sh::swatch};
    if (dashed) {
        ui::drawFrame(g, box, ink, ui::theme::stroke::rule, true);
        return;
    }
    g.setColour(ink);
    g.fillRect(box);
}

juce::Colour inkOf(const live::Out& out) { return out.main() ? colour::text : colour::identity(Product::Decoder); }

}  // namespace

void IoPage::show(Shown shown) {
    s_ = std::move(shown);
    if (std::none_of(s_.inputs.begin(), s_.inputs.end(), [&](const live::HostInput& i) { return i.id == input_; }))
        input_ = s_.inputs.empty() ? 0 : s_.inputs.front().id;
    repaint();
}

void IoPage::setLoad(float load) {
    s_.load = load;
    if (isVisible() && tab_ == Tab::Interface) repaint();
}

void IoPage::setLead(double ms, double spreadMs, int count) {
    s_.leadMs = ms;
    s_.leadSpreadMs = spreadMs;
    s_.leadCount = count;
    if (isVisible() && tab_ == Tab::Midi) repaint();
}

bool IoPage::keyPressed(const juce::KeyPress& key) {
    if (key != juce::KeyPress::escapeKey) return false;
    const bool waited = armed_.escape();
    if (!waited && !menu_) return false;
    menu_ = false;
    repaint();
    return true;
}

void IoPage::edit(const std::function<void(live::AudioSettings&)>& how) {
    live::AudioSettings next = s_.settings;
    how(next);
    s_.settings = next;
    if (change) change(next);
    repaint();
}

live::Row IoPage::outputRow() const { return live::rowOf(live::aggregateOf(s_.settings.devices, s_.system)); }
live::Row IoPage::inputRow() const { return live::rowOf(live::aggregateOf(s_.settings.devices, s_.system), true); }

juce::String IoPage::where(const live::Row& row, int at, int width) const {
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

void IoPage::paint(juce::Graphics& g) {
    g.fillAll(colour::ground);
    clearRegions();
    const auto top = static_cast<float>(ui::theme::metrics::headerHeight);
    setViewport(top, static_cast<float>(getHeight()));

    //  under the band, its edges where the band's are, as the other pages' are; what is taller scrolls under it
    float bottom = top;
    {
        const ContentClip clip(*this, g);
        auto page = getLocalBounds().toFloat().withTrimmedTop(top).reduced(ui::theme::metrics::bandInset, sh::pad);
        page.translate(0.0f, -scroll());
        armed_.ground(*this, getLocalBounds().toFloat(), [this] { repaint(); });
        if (s_.problem.isNotEmpty()) {
            ui::text(g, s_.problem, ui::labelFont(), colour::text, page.removeFromTop(sh::line),
                     juce::Justification::centredLeft);
        }
        if (tab_ == Tab::Interface)
            bottom = interfacePage(g, page);
        else if (tab_ == Tab::Outputs)
            bottom = outputsPage(g, page);
        else if (tab_ == Tab::Inputs)
            bottom = inputsPage(g, page);
        else
            bottom = midiPage(g, page);
    }
    noteContentBottom(bottom + scroll() + sh::pad);

    //  the band over what scrolls: the tabs, with what each holds, as the other pages carry their own controls there
    const auto band = ui::drawPageBand(g, static_cast<float>(getWidth()));
    const float tabHeight = ctl::tabBarHeight - ctl::matrixTabsTop;
    const auto row = outputRow();
    const std::vector<juce::String> names{
        "interface " + juce::String(static_cast<int>(live::aggregateOf(s_.settings.devices, s_.system).size())),
        "outputs " + juce::String(row.width()), "inputs " + juce::String(static_cast<int>(s_.inputs.size())),
        "midi " + juce::String(static_cast<int>(s_.midiInputs.size()))};
    paintTabRowAt(g, band.getX(), band.getCentreY() - tabHeight / 2.0f, tabHeight, names, static_cast<int>(tab_),
                  [this](int t) {
                      tab_ = static_cast<Tab>(t);
                      armed_.end();
                      menu_ = false;
                      scrollTo(0.0f);
                      repaint();
                  });
}

// ---- interface ----------------------------------------------------------------------------------

float IoPage::interfacePage(juce::Graphics& g, juce::Rectangle<float> page) {
    float y = ui::drawGroupHeading(g, page.getX(), page.getY(), page.getWidth(), "devices");
    const auto& chosen = s_.settings.devices;
    const auto joined = live::aggregateOf(chosen, s_.system);
    for (std::size_t k = 0; k < chosen.size(); ++k) {
        const std::string uid = chosen[k];
        const auto d =
            std::find_if(joined.begin(), joined.end(), [&](const live::AudioDevice& x) { return x.uid == uid; });
        const bool here = d != joined.end();
        const auto tile = gridCell(page, y, static_cast<int>(k), sh::deviceColumns);
        ui::TileContent t;
        t.value = here ? juce::String(d->name) : juce::String(uid);
        t.name = here ? (k == 0 ? juce::String("the clock") : juce::String("drift corrected")) + kSeparator +
                            juce::String(d->outs) + " out" + kSeparator + juce::String(d->ins) + " in" + kSeparator +
                            ms(d->outLatency, s_.rate)
                      : juce::String("not here, or past what the row holds");
        t.enabled = here;
        t.bar = false;
        ui::drawTile(g, tile, t);
        const juce::Rectangle<float> x{tile.getRight() - ctl::closeGlyph, tile.getY(), ctl::closeGlyph,
                                       ctl::closeGlyph};
        addClickArea(ui::drawCloseCross(g, x), [this, k] {
            edit([k](live::AudioSettings& s) { s.devices.erase(s.devices.begin() + static_cast<long>(k)); });
        });
        if (k > 0 && here) {
            const auto box = ui::tileBox(tile);
            const auto make = button(g, box.getRight() - sh::itemPad - ui::framedButtonWidth("make clock"),
                                     box.getCentreY(), "make clock");
            addClickArea(make, [this, k] {
                edit([k](live::AudioSettings& s) {
                    std::rotate(s.devices.begin(), s.devices.begin() + static_cast<long>(k),
                                s.devices.begin() + static_cast<long>(k) + 1);
                });
            });
        }
    }
    y = gridBottom(y, static_cast<int>(chosen.size()), sh::deviceColumns) + ctl::segmentGap;
    const auto add = button(g, page.getX(), y + sh::control / 2.0f, "+ device");
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
                 ui::smallFont(), colour::label, {page.getX(), y, page.getWidth(), sh::noteLine},
                 juce::Justification::centredLeft);
        y += sh::noteLine;
    }

    //  the timing and the order, a tile each; the two choices under their tiles
    y = ui::drawGroupHeading(g, page.getX(), y + sh::gap, page.getWidth(), "timing and order");
    const std::vector<juce::String> buffers{"32", "64", "128", "256"};
    const std::vector<juce::String> orders{"1", "2", "3", "4", "5", "6", "7"};
    const int chosenBuffer =
        static_cast<int>(std::find(buffers.begin(), buffers.end(), juce::String(s_.settings.buffer)) - buffers.begin());
    const int field = (s_.order + 1) * (s_.order + 1);
    const float share = std::clamp(s_.load, 0.0f, 1.0f);
    ui::TileContent rate, buffer, order, load;
    rate.name = "rate" + kSeparator + "Live's";
    rate.value = juce::String(s_.rate / 1000.0, juce::roundToInt(s_.rate) % 1000 == 0 ? 0 : 1) + " kHz";
    rate.bar = false;
    buffer.name = "buffer" + kSeparator + ms(s_.settings.buffer, s_.rate);
    buffer.value = juce::String(s_.settings.buffer) + " samples";
    buffer.fillTo = static_cast<double>(std::max(chosenBuffer, 0)) / static_cast<double>(buffers.size() - 1);
    order.name = "order" + kSeparator + "every plugin" + kSeparator + "with the set";
    order.value = juce::String(s_.order) + kSeparator + juce::String(field) + " channels";
    order.fillTo = static_cast<double>(s_.order - 1) / static_cast<double>(orders.size() - 1);
    load.name = "load" + kSeparator + "this machine, now";
    load.value = juce::String(juce::roundToInt(100.0f * share)) + "% of each block";
    load.fillTo = share;
    const std::vector<ui::TileContent> tiles{rate, buffer, order, load};
    for (std::size_t i = 0; i < tiles.size(); ++i)
        ui::drawTile(g, gridCell(page, y, static_cast<int>(i), sh::settingColumns), tiles[i]);
    const auto under = [&](int index) {
        const auto box = ui::tileBox(gridCell(page, y, index, sh::settingColumns));
        return juce::Rectangle<float>{box.getX(), box.getBottom() + ctl::segmentGap, box.getWidth(), sh::control};
    };
    const auto cells = ui::drawSegments(g, under(1), buffers, chosenBuffer, true);
    for (std::size_t i = 0; i < cells.size(); ++i)
        addClickArea(cells[i],
                     [this, v = buffers[i].getIntValue()] { edit([v](live::AudioSettings& s) { s.buffer = v; }); });
    const auto orderCells = ui::drawSegments(g, under(2), orders, s_.order - 1, true);
    for (std::size_t i = 0; i < orderCells.size(); ++i)
        addClickArea(orderCells[i], [this, next = static_cast<int>(i) + 1] {
            if (next != s_.order && changeOrder) changeOrder(next);
        });
    y = under(1).getBottom() + sh::gap;

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
        y = std::max(y, menu.box.getBottom());
    }
    return y;
}

// ---- outputs ------------------------------------------------------------------------------------

juce::String IoPage::nameOf(const live::Out& out) const {
    if (out.main()) return "Live's Main";
    const juce::String which = out.which == Output::Headphones ? "headphones" : "speakers";
    return s_.decoders.size() > 1 ? decoderName(out.decoder) + " " + which : which;
}

juce::String IoPage::decoderName(std::uint64_t id) const {
    for (std::size_t k = 0; k < s_.decoders.size(); ++k)
        if (s_.decoders[k].id == id) return "decoder " + juce::String(static_cast<int>(k) + 1);
    return "a decoder";
}

float IoPage::outputItem(juce::Graphics& g, float x, float y, float width, const live::Out& out) {
    const auto row = outputRow();
    const juce::Rectangle<float> box{x, y, width, sh::itemHeight};
    itemFrame(g, box, out == chosen_);
    if (!armed_.on())
        addClickArea(box, [this, out] {
            chosen_ = out;
            repaint();
        });
    const auto inner = box.reduced(sh::itemPad);
    const juce::Rectangle<float> head{inner.getX(), inner.getY(), inner.getWidth(), sh::noteLine};
    swatch(g, head.getX(), head.getCentreY(), inkOf(out), false);
    const juce::String what = out.main()                        ? juce::String("Live's Main")
                              : out.which == Output::Headphones ? "headphones"
                                                                : "speakers";
    ui::text(g, what, ui::labelFont(), colour::text, head.withTrimmedLeft(sh::swatch + sh::itemPad),
             juce::Justification::centredLeft);

    //  how wide it is, and for the speakers the Decoder's layout; where it plays
    const auto bus = live::busChannels(out, s_.decoders);
    const int width2 = bus.empty() ? 0 : bus.back() - bus.front() + 1;
    juce::String size = juce::String(width2) + " ch";
    if (!out.main() && out.which == Output::Speakers)
        for (const auto& d : s_.decoders)
            if (d.id == out.decoder && !d.report.layoutName.empty())
                size += kSeparator + juce::String(d.report.layoutName);
    const auto taken = live::takenBy(out, s_.settings.at(out), row, s_.decoders);
    const juce::String at = taken.empty() ? juce::String("placed nowhere: not heard")
                                          : where(row, taken.front(), taken.back() - taken.front() + 1);
    ui::text(g, size + kSeparator + at, ui::smallFont(), taken.empty() ? colour::text : colour::label, head,
             juce::Justification::centredRight);

    //  the ways to make a copy, or the copy waiting for its channel
    const float cy = inner.getBottom() - sh::control / 2.0f;
    if (armed_.what() && *armed_.what() == out) {
        const juce::String waits = "click a channel";
        armed_.block(*this, g, {inner.getX(), cy - sh::control / 2.0f, ui::Armed<live::Out>::width(waits), sh::control},
                     waits, [this] { repaint(); });
        return box.getBottom();
    }
    const auto copy = button(g, inner.getX(), cy, "+ copy");
    const bool backAlready = std::any_of(s_.settings.copies.begin(), s_.settings.copies.end(),
                                         [&](const live::Copy& c) { return c.of == out && c.toLive; });
    const bool offersBack = !out.main() && bus.size() == 2 && !backAlready;
    const auto back =
        offersBack ? button(g, copy.getRight() + ctl::segmentGap, cy, "+ back to Live") : juce::Rectangle<float>{};
    if (armed_.on()) {
        armed_.over(*this, g, box, false, {});
        return box.getBottom();
    }
    addClickArea(copy, [this, out] {
        armed_.begin(out);
        chosen_ = out;
        repaint();
    });
    if (offersBack)
        addClickArea(back,
                     [this, out] { edit([out](live::AudioSettings& s) { s.copies.push_back({out, {}, true}); }); });
    return box.getBottom();
}

float IoPage::copyItem(juce::Graphics& g, float x, float y, float width, std::size_t index) {
    const auto row = outputRow();
    const live::Copy& c = s_.settings.copies[index];
    const juce::Rectangle<float> box{x, y, width, sh::copyHeight};
    itemFrame(g, box, false);
    const auto inner = box.reduced(sh::itemPad);
    const juce::Rectangle<float> head{inner.getX(), inner.getY(), inner.getWidth(), sh::noteLine};
    swatch(g, head.getX(), head.getCentreY(), inkOf(c.of), true);
    ui::text(g, "copy of " + nameOf(c.of), ui::labelFont(), colour::text,
             head.withTrimmedLeft(sh::swatch + sh::itemPad).withTrimmedRight(ctl::closeGlyph + sh::itemPad),
             juce::Justification::centredLeft);
    const juce::Rectangle<float> cross{head.getRight() - ctl::closeGlyph, head.getCentreY() - ctl::closeGlyph / 2.0f,
                                       ctl::closeGlyph, ctl::closeGlyph};
    const auto hit = ui::drawCloseCross(g, cross);
    if (!armed_.on())
        addClickArea(hit, [this, index] {
            edit([index](live::AudioSettings& s) { s.copies.erase(s.copies.begin() + static_cast<long>(index)); });
        });
    const auto copied = live::takenBy(c.of, c.at, row, s_.decoders);
    const juce::String at = c.toLive         ? juce::String("back to Live, on the bambi track")
                            : copied.empty() ? juce::String("nowhere")
                                             : where(row, copied.front(), copied.back() - copied.front() + 1);
    ui::text(g, at, ui::smallFont(), colour::label,
             {head.getX() + sh::swatch + sh::itemPad, head.getBottom(), head.getWidth(), sh::noteLine},
             juce::Justification::centredLeft);
    armed_.over(*this, g, box, false, {});
    return box.getBottom();
}

float IoPage::outputsPage(juce::Graphics& g, juce::Rectangle<float> page) {
    const auto row = outputRow();
    const float listX = page.getX(), listWidth = sh::listWidth;
    const float cardsX = listX + listWidth + sh::gap, cardsWidth = page.getRight() - cardsX;

    //  the list: Live's Main, each Decoder's outputs that play, then the copies
    float y = outputItem(g, listX, page.getY(), listWidth, live::kLiveMain) + sh::itemGap;
    for (const live::PlayingDecoder& d : s_.decoders) {
        const juce::Rectangle<float> head{listX, y, listWidth, std::max(sh::noteLine, sh::control)};
        ui::text(g, decoderName(d.id), ui::smallFont(), colour::label, head, juce::Justification::centredLeft);
        const auto open = button(g, head.getRight() - ui::framedButtonWidth("open"), head.getCentreY(), "open");
        if (armed_.on())
            armed_.over(*this, g, open, false, {});
        else
            addClickArea(open, [this, id = d.id] {
                if (openDecoder) openDecoder(id);
            });
        y = head.getBottom() + sh::itemGap;
        bool any = false;
        for (const Output which : {Output::Headphones, Output::Speakers}) {
            const live::Out out{d.id, which};
            if (live::busChannels(out, s_.decoders).empty()) continue;
            y = outputItem(g, listX, y, listWidth, out) + sh::itemGap;
            any = true;
        }
        if (!any) {
            ui::text(g, "it plays nothing" + kSeparator + "set on its page", ui::smallFont(), colour::label,
                     {listX, y, listWidth, sh::noteLine}, juce::Justification::centredLeft);
            y += sh::noteLine + sh::itemGap;
        }
    }
    if (!s_.settings.copies.empty()) {
        ui::text(g, "copies", ui::smallFont(), colour::label, {listX, y, listWidth, sh::noteLine},
                 juce::Justification::centredLeft);
        y += sh::noteLine + sh::itemGap;
        for (std::size_t i = 0; i < s_.settings.copies.size(); ++i)
            y = copyItem(g, listX, y, listWidth, i) + sh::itemGap;
    }

    //  the devices, each a card of its output channels
    float ry = page.getY();
    if (row.width() == 0) {
        ui::text(g, "No device chosen: the headphones go back to Live, on the bambi track. Choose one on interface.",
                 ui::labelFont(), colour::label, {cardsX, ry, cardsWidth, sh::line}, juce::Justification::centredLeft);
        return std::max(y, ry + sh::line);
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
    //  short, to fit under its run: "main", "dec 1", and which output when a Decoder plays both
    const auto shortName = [&](const live::Out& out) {
        if (out.main()) return juce::String("main");
        const bool both = !live::busChannels({out.decoder, Output::Headphones}, s_.decoders).empty() &&
                          !live::busChannels({out.decoder, Output::Speakers}, s_.decoders).empty();
        const juce::String name = decoderName(out.decoder).replace("decoder", "dec");
        return both ? name + (out.which == Output::Headphones ? " hp" : " sp") : name;
    };
    const auto place = [&](const live::Out& out, const live::DeviceChannel& at, bool host) {
        const auto taken = live::takenBy(out, at, row, s_.decoders);
        if (taken.empty()) return;
        blocks.push_back({taken, labelsOf(out, taken), host ? juce::String("copy") : shortName(out), host, inkOf(out)});
    };

    place(live::kLiveMain, s_.settings.main, false);
    for (const live::PlayingDecoder& d : s_.decoders)
        for (const Output which : {Output::Headphones, Output::Speakers})
            place({d.id, which}, s_.settings.at({d.id, which}), false);
    for (const live::Copy& c : s_.settings.copies)
        if (!c.toLive) place(c.of, c.at, true);
    const juce::String what =
        armed_.what() ? "a click on a channel puts a copy of " + nameOf(*armed_.what()) + " there"
                      : "a click on a channel puts " + nameOf(chosen_) + " there, on whichever device it is";
    ui::text(g, what, ui::labelFont(), colour::label, {cardsX, ry, cardsWidth, sh::line},
             juce::Justification::centredLeft);
    ry = cards(g, cardsX, ry + sh::line, cardsWidth, row, blocks, [this, row](int c) {
        if (armed_.what()) {
            const live::Out of = *armed_.what();
            armed_.end();
            edit([&](live::AudioSettings& s) { s.copies.push_back({of, row.channel(c), false}); });
            return;
        }
        const live::Out out = chosen_;
        edit([&](live::AudioSettings& s) {
            if (out.main()) {
                s.main = row.channel(c);
                return;
            }
            const auto had =
                std::find_if(s.placed.begin(), s.placed.end(), [&](const live::Placed& x) { return x.out == out; });
            if (had != s.placed.end())
                had->at = row.channel(c);
            else
                s.placed.push_back({out, row.channel(c)});
        });
    });
    ui::text(g, "A copy is the host's, after the decoder. Live renders nothing that does not come back to it.",
             ui::smallFont(), colour::label, {cardsX, ry, cardsWidth, sh::noteLine}, juce::Justification::centredLeft);
    return std::max(y, ry + sh::noteLine);
}

// ---- the cards ----------------------------------------------------------------------------------

float IoPage::cards(juce::Graphics& g, float x, float top, float width, const live::Row& row,
                    const std::vector<Block>& blocks, const std::function<void(int)>& place) {
    const float pitch = cs::cell + cs::cellGap;
    int c = 1;
    float y = top;
    for (std::size_t k = 0; k < row.devices.size(); ++k) {
        const live::AudioDevice& d = row.devices[k];
        const int count = row.inputs ? d.ins : d.outs;
        if (count == 0) continue;
        //  the channels first, so the card is as tall as their lines
        const float left = x + sh::itemPad, stripTop = y + sh::itemPad + sh::noteLine;
        const auto strip =
            ui::ChannelStrip::wrapped(left + sh::stripLabel, stripTop, width - 2.0f * sh::itemPad - sh::stripLabel,
                                      count, cs::cell + sh::caption);
        const juce::Rectangle<float> card{x, y, width, strip.bottom + sh::caption + sh::itemPad - y};
        ui::drawFrame(g, card, colour::ruleStrong, ui::theme::stroke::rule, false);
        auto head = juce::Rectangle<float>{left, y + sh::itemPad, width - 2.0f * sh::itemPad, sh::noteLine};
        const juce::String name(d.name);
        ui::text(g, name, ui::labelFont(), colour::text,
                 head.removeFromLeft(ui::textWidth(ui::labelFont(), name) + sh::itemPad),
                 juce::Justification::centredLeft);
        ui::text(g,
                 juce::String(d.outs) + " out" + kSeparator + juce::String(d.ins) + " in" + kSeparator +
                     (k == 0 ? "the clock" : "drift corrected") + kSeparator +
                     ms(row.inputs ? d.inLatency : d.outLatency, s_.rate),
                 ui::smallFont(), colour::label, head, juce::Justification::centredLeft);
        ui::text(g, row.inputs ? "in" : "out", ui::smallFont(), colour::label,
                 {left, stripTop, sh::stripLabel, cs::cell}, juce::Justification::centredLeft);
        const int first = c;
        for (int i = 0; i < count; ++i, ++c) {
            const auto box = strip.cells[static_cast<std::size_t>(i)];
            std::vector<std::pair<const Block*, std::size_t>> here;
            for (const Block& b : blocks)
                for (std::size_t n = 0; n < b.channels.size(); ++n)
                    if (b.channels[n] == c) here.push_back({&b, n});
            const Block* owner = here.empty() ? nullptr : here.front().first;
            //  in its owner's colour; the host's own copy dashed in it, and empty
            ui::ChannelCell cell{juce::String(i + 1), {}, {}, static_cast<int>(here.size())};
            if (owner != nullptr) {
                const std::size_t n = here.front().second;
                cell.label = n < owner->labels.size() ? owner->labels[n] : juce::String();
                if (owner->host)
                    cell.dashed = owner->ink;
                else
                    cell.fill = owner->ink;
            }
            ui::ChannelStrip::paintCell(g, box, cell);
            //  under the first channel of each run: what plays there, as far as the next run on its line
            for (const auto& [b, n] : here) {
                if (n != 0 || b->caption.isEmpty()) continue;
                int next = c + 1;
                while (next < first + count &&
                       std::abs(strip.cells[static_cast<std::size_t>(next - first)].getY() - box.getY()) < 0.5f &&
                       std::none_of(
                           blocks.begin(), blocks.end(),
                           [&](const Block& o) { return !o.channels.empty() && o.channels.front() == next; }))
                    ++next;
                ui::text(g, b->caption, ui::smallFont(), colour::label,
                         {box.getX(), box.getBottom(), static_cast<float>(next - c) * pitch - cs::cellGap, sh::caption},
                         juce::Justification::centredLeft);
            }
            if (armed_.on())
                armed_.over(*this, g, box, true, [place, c] { place(c); });
            else
                addClickArea(box, [place, c] { place(c); });
        }
        y = card.getBottom() + sh::itemGap;
    }
    return y;
}

// ---- inputs -------------------------------------------------------------------------------------

float IoPage::inputsPage(juce::Graphics& g, juce::Rectangle<float> page) {
    const auto row = inputRow();
    const float listX = page.getX(), listWidth = sh::listWidth;
    const float cardsX = listX + listWidth + sh::gap, cardsWidth = page.getRight() - cardsX;
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

    //  the list: each input, how wide, and where it is heard
    float y = page.getY();
    for (const live::HostInput& in : s_.inputs) {
        const juce::Rectangle<float> box{listX, y, listWidth, sh::itemHeight};
        itemFrame(g, box, in.id == input_);
        addClickArea(box, [this, id = in.id] {
            input_ = id;
            repaint();
        });
        const auto inner = box.reduced(sh::itemPad);
        const juce::Rectangle<float> head{inner.getX(), inner.getY(), inner.getWidth(), sh::noteLine};
        swatch(g, head.getX(), head.getCentreY(), colour::label, false);
        ui::text(g, juce::String(in.name), ui::labelFont(), colour::text,
                 head.withTrimmedLeft(sh::swatch + sh::itemPad), juce::Justification::centredLeft);
        const int at = atOf(in.id);
        const bool oneDevice = at != 0 && row.deviceOf(at) == row.deviceOf(at + in.width - 1);
        ui::text(g,
                 at == 0     ? juce::String("nowhere")
                 : oneDevice ? where(row, at, in.width)
                             : juce::String("not on one device: not heard"),
                 ui::smallFont(), oneDevice || at == 0 ? colour::label : colour::text, head,
                 juce::Justification::centredRight);
        const std::vector<juce::String> widths{"mono", "stereo"};
        const auto cells =
            ui::drawSegments(g, {inner.getX(), inner.getBottom() - sh::control, ui::segmentsWidth(widths), sh::control},
                             widths, in.width - 1, true);
        for (std::size_t i = 0; i < cells.size(); ++i)
            addClickArea(cells[i], [this, changeAll, id = in.id, width = static_cast<int>(i) + 1] {
                auto inputs = s_.inputs;
                for (auto& x : inputs)
                    if (x.id == id) x.width = width;
                changeAll(inputs, s_.settings);
            });
        const juce::Rectangle<float> cross{inner.getRight() - ctl::closeGlyph,
                                           inner.getBottom() - sh::control / 2.0f - ctl::closeGlyph / 2.0f,
                                           ctl::closeGlyph, ctl::closeGlyph};
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
        y = box.getBottom() + sh::itemGap;
    }
    const auto add = button(g, listX, y + sh::control / 2.0f, "+ input");
    addClickArea(add, [this, changeAll, row] {
        auto inputs = s_.inputs;
        auto settings = s_.settings;
        live::TrackId id = live::kFirstInput;
        for (const auto& x : inputs) id = std::max(id, x.id + 1);
        //  the first channel no input takes yet
        int free = 1;
        while (free < row.width() &&
               std::any_of(
                   settings.inputs.begin(), settings.inputs.end(), [&](const auto& x) { return row.at(x.at) == free; }))
            ++free;
        inputs.push_back({id, "input " + std::to_string(inputs.size() + 1), 1});
        settings.inputs.push_back({id, row.channel(free)});
        input_ = id;
        changeAll(inputs, settings);
    });
    y = add.getBottom() + sh::itemGap;

    //  the devices, each a card of its input channels
    float ry = page.getY();
    if (row.width() == 0) {
        ui::text(g, "None of the chosen devices has inputs.", ui::labelFont(), colour::label,
                 {cardsX, ry, cardsWidth, sh::line}, juce::Justification::centredLeft);
        return std::max(y, ry + sh::line);
    }
    std::vector<Block> blocks;
    for (const live::HostInput& in : s_.inputs) {
        const int at = atOf(in.id);
        if (at == 0) continue;
        Block b;
        for (int k = 0; k < in.width; ++k) b.channels.push_back(at + k);
        b.labels = in.width == 2 ? std::vector<juce::String>{"L", "R"}
                                 : std::vector<juce::String>{juce::String(in.name).substring(0, 3)};
        b.caption = juce::String(in.name);
        b.ink = colour::label;
        blocks.push_back(std::move(b));
    }
    const auto chosen =
        std::find_if(s_.inputs.begin(), s_.inputs.end(), [&](const live::HostInput& i) { return i.id == input_; });
    ui::text(g,
             chosen == s_.inputs.end()
                 ? juce::String("an input is a source of the field, beside Live's tracks")
                 : "a click on a channel puts " + juce::String(chosen->name) + " there, on whichever device it is",
             ui::labelFont(), colour::label, {cardsX, ry, cardsWidth, sh::line}, juce::Justification::centredLeft);
    ry = cards(g, cardsX, ry + sh::line, cardsWidth, row, blocks, [this, changeAll, row](int c) {
        if (input_ == 0) return;
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
    ui::text(g, "An input is heard at the least latency, with an Encoder, inserts and sends as any track's.",
             ui::smallFont(), colour::label, {cardsX, ry, cardsWidth, sh::noteLine}, juce::Justification::centredLeft);
    return std::max(y, ry + sh::noteLine);
}

// ---- midi ---------------------------------------------------------------------------------------

float IoPage::midiPage(juce::Graphics& g, juce::Rectangle<float> page) {
    const float top = page.getY();
    int index = 0;
    for (std::size_t k = 0; k < s_.midiInputs.size(); ++k, ++index) {
        ui::TileContent t;
        t.name = k + 1 == s_.midiInputs.size() ? "midi in" + kSeparator + "the host's own: Live's MIDI tracks"
                                               : juce::String("midi in");
        t.value = s_.midiInputs[k];
        t.nameActive = true;
        t.bar = false;
        ui::drawTile(g, gridCell(page, top, index, sh::deviceColumns), t);
    }
    if (s_.midiInputs.empty()) {
        ui::TileContent t;
        t.name = "midi in";
        t.value = "none";
        t.enabled = false;
        t.bar = false;
        ui::drawTile(g, gridCell(page, top, index++, sh::deviceColumns), t);
    }

    //  Live's MIDI, held back to meet its audio: a tile, its steps and "measure" under it
    ui::TileContent lead;
    lead.name = "Live's MIDI held back";
    lead.value = juce::String(s_.settings.liveMidiMs, 1) + " ms";
    lead.fillTo = s_.settings.liveMidiMs / live::kMaxLiveMidiMs;
    const auto tile = gridCell(page, top, index, sh::deviceColumns);
    ui::drawTile(g, tile, lead);
    const auto box = ui::tileBox(tile);
    const std::vector<juce::String> steps{kDash, "+", "measure"};
    const auto cells = ui::drawSegments(g, {box.getX(), box.getBottom() + ctl::segmentGap, box.getWidth(), sh::control},
                                        steps, s_.measuring ? 2 : -1, true);
    const auto step = [this](double by) {
        edit([by](live::AudioSettings& a) {
            a.liveMidiMs = std::clamp(std::round(a.liveMidiMs + by), 0.0, live::kMaxLiveMidiMs);
        });
    };
    addClickArea(cells[0], [step] { step(-1.0); });
    addClickArea(cells[1], [step] { step(1.0); });
    addClickArea(cells[2], [this] {
        s_.measuring = !s_.measuring;
        if (measure) measure(s_.measuring);
        repaint();
    });
    float y = std::max(gridBottom(top, index + 1, sh::deviceColumns), cells[0].getBottom() + sh::gap);

    if (s_.leadCount > 0) {
        const juce::String found = "measured " + juce::String(s_.leadMs, 1) + " ms " +
                                   juce::String::fromUTF8("\xc2\xb1") + " " + juce::String(s_.leadSpreadMs, 1) +
                                   kSeparator + juce::String(s_.leadCount) + (s_.leadCount == 1 ? " note" : " notes");
        const float w = ui::textWidth(ui::labelFont(), found) + sh::itemPad;
        ui::text(g, found, ui::labelFont(), colour::text, {page.getX(), y, w, sh::line},
                 juce::Justification::centredLeft);
        const double value = std::clamp(s_.leadMs, 0.0, live::kMaxLiveMidiMs);
        const auto use = button(g, page.getX() + w, y + sh::line / 2.0f, "use");
        addClickArea(use, [this, value] { edit([value](live::AudioSettings& a) { a.liveMidiMs = value; }); });
        y += sh::line;
    }
    ui::text(g,
             s_.measuring ? "Listening: a MIDI track sending to bambi, and on Main a click on its notes, nothing else."
                          : "Measure with a MIDI track sending to bambi and, on Main, a click on its notes.",
             ui::smallFont(), colour::label, {page.getX(), y, page.getWidth(), sh::noteLine},
             juce::Justification::centredLeft);
    y += sh::noteLine;
    ui::text(g, "Every plugin hears all of it, and answers to the channel and note it learned.", ui::smallFont(),
             colour::label, {page.getX(), y, page.getWidth(), sh::noteLine}, juce::Justification::centredLeft);
    return y + sh::noteLine;
}

}  // namespace bambi::livehost
