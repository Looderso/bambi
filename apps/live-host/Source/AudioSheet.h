// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <optional>
#include <vector>

#include "bambi/decode/remote.hpp"
#include "bambi/live/field.hpp"
#include "bambi/live/io.hpp"
#include "bambi/ui/Sheet.h"
#include "bambi/ui/Theme.h"

/*  The live host's audio sheet, over its window: where it plays and hears on this computer, in three tabs.
 *
 *    - interface: the devices joined with the loop device, the clock first, each with its channels and latency;
 *      "+ device" offers the system's others, a wireless one listed and not offered. Live's rate, shown; the host's
 *      buffer; the order, beside the share of each block the host takes as it plays;
 *    - outputs: Live's Main, and each Decoder's outputs that play -- which, and how, is the Decoder's, set on its
 *      own page -- as blocks of channels to place, each with its copies, the host's: on other channels, or a pair
 *      back to Live for its bambi track to record. Then the channel map: every device a group, what plays on each
 *      channel, the host's own dashed, two things on one channel marked. A click on the map puts the chosen output
 *      there, from that channel; "+ copy" arms, and the click puts a copy there instead;
 *    - inputs: the host's own sources of the field, each heard from a device channel, one or two wide; the input
 *      map places the chosen one.
 *
 *  It changes the settings, never the engine: `change` and `changeInputs` say what the user asked, and whoever
 *  owns the engine applies it and shows the sheet again. Clicks on release; escape cancels an armed copy or the
 *  menu, else closes. Message thread.
 */
namespace bambi::livehost {

class AudioSheet final : public ui::Sheet {
public:
    struct Shown {
        live::AudioSettings settings;
        std::vector<live::AudioDevice> system;
        std::vector<live::PlayingDecoder> decoders;  ///< every Decoder, where it has its outputs now
        std::vector<live::HostInput> inputs;
        double rate{48000.0};
        int order{3};
        float load{0.0f};
        juce::String problem;  ///< what went wrong applying the last change, or empty
    };
    void show(Shown shown);
    void setLoad(float load);

    std::function<void(live::AudioSettings)> change;
    std::function<void(std::vector<live::HostInput>, live::AudioSettings)> changeInputs;
    std::function<void(std::uint64_t)> openDecoder;  ///< a Decoder's own page, by its id

    enum class Tab { Interface, Outputs, Inputs };
    /// The tab it shows, as a click on its head chooses it. For checks.
    void showTab(Tab tab) {
        tab_ = tab;
        repaint();
    }

    void paint(juce::Graphics& g) override;

private:
    void dismiss() override;
    void edit(const std::function<void(live::AudioSettings&)>& how);
    juce::Rectangle<float> box() const;
    float interfacePage(juce::Graphics& g, juce::Rectangle<float> page);
    float outputsPage(juce::Graphics& g, juce::Rectangle<float> page);
    float inputsPage(juce::Graphics& g, juce::Rectangle<float> page);
    void outputRow(juce::Graphics& g, juce::Rectangle<float> row, const live::Out& out);
    juce::String nameOf(const live::Out& out) const;
    juce::String decoderName(std::uint64_t id) const;

    //  A block on the map: channels, what each is called there, and how it is drawn.
    struct Block {
        std::vector<int> channels;  ///< row channels, from 1
        std::vector<juce::String> labels;
        bool host{false};    ///< the host's own: dashed
        bool chosen{false};  ///< the output the map places
        juce::Colour ink{ui::theme::colour::identity(Product::Decoder)};  ///< its frame: whose it is
    };
    /// The map of `row`, from `top`; a click on a channel calls `place` with it. Returns its bottom.
    float map(juce::Graphics& g, float x, float top, float width, const live::Row& row,
              const std::vector<Block>& blocks, std::function<void(int)> place);

    live::Row outputRow() const;
    live::Row inputRow() const;
    juce::String where(const live::Row& row, int at, int width) const;

    Shown s_;
    Tab tab_{Tab::Outputs};
    live::Out chosen_{live::kFirstDecoder, Output::Headphones};  ///< the output the map places
    std::optional<live::Out> arming_;  ///< "+ copy" pressed: the next click on the map places a copy
    bool menu_{false};
    live::TrackId input_{0};  ///< the input the input map places
};

}  // namespace bambi::livehost
