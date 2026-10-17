// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

#include "bambi/decode/remote.hpp"
#include "bambi/live/field.hpp"
#include "bambi/patch/json.hpp"

/*  Where the live host plays everything out, and hears its own inputs. The devices the user chose lie side by side as one row of output
 *  channels, the clock's first; the Decoder's bus is that row, channel for channel, so where it places an output is
 *  where it is heard. Live's Main and every copy of an output are the host's: it keeps their channels, the Decoder
 *  stays clear of them, and the host plays them itself, after the Decoder.
 *
 *  Every route is delayed so that all of it is heard together: by how much sooner its device plays than the
 *  slowest chosen, and Live's Main further by the field's latency, so a track left in Live lines up with one the
 *  field plays. Channels are named by their device and number there, never by their place in the row, so a
 *  setting survives a device added before it.
 *
 *  The same devices' inputs lie side by side as a second row, after the loop device's channels on the host's input
 *  row (graph.hpp): an input of the field arrives there, on the device channel this computer keeps for it, and is
 *  heard as soon as it arrives -- never held back to meet Live's tracks, which come a buffer of Live's later.
 *
 *  Pure functions of what they are given, for the message thread. Channels and latencies are counted from 1 and
 *  in samples at Live's rate.
 */
namespace bambi::live {

/// The most channels the row holds: the Decoder's bus is the row, and a VST3 bus stops here.
inline constexpr int kMaxRowChannels = kMaxVst3BusChannels;

/// One of the system's audio devices, as the host finds it.
struct AudioDevice {
    std::string uid;  ///< the system's own, the same from one start to the next
    std::string name;
    int outs{0}, ins{0};
    int outLatency{0};  ///< from a sample handed over to its sound, as the device reports it
    int inLatency{0};   ///< from a sound to its sample handed over
    /// Bluetooth, or a phone's microphone over the air: listed, never offered -- its latency, and a Bluetooth
    /// device's rate moving when its microphone opens.
    bool wireless{false};

    friend bool operator==(const AudioDevice&, const AudioDevice&) = default;
};

/// A channel of a device, by the device's uid and the channel's number there.
struct DeviceChannel {
    std::string device;
    int number{0};  ///< 0 for none

    bool none() const { return number == 0; }
    friend bool operator==(const DeviceChannel&, const DeviceChannel&) = default;
};

/// The chosen devices' outputs, or their inputs, as one row.
struct Row {
    std::vector<AudioDevice> devices;  ///< as chosen, the clock first
    bool inputs{false};                ///< a row of the devices' inputs; otherwise of their outputs

    int width() const;                     ///< every device's channels
    int at(const DeviceChannel& c) const;  ///< its row channel, or 0 when its device is not on the row
    DeviceChannel channel(int at) const;   ///< the device channel at a row channel; none past the row
    int deviceOf(int at) const;            ///< the index of the device a row channel is on, or -1
    std::vector<int> deviceEnds() const;   ///< each device's last row channel but the last's
    /// What the Decoder's placement keeps to: the row's width and its devices, the channels given kept.
    OutputBus bus(std::vector<int> kept) const;
};
/// The devices the host joins with the loop device: those `chosen`, in order, that the system has and offers, each
/// once, while every one's outputs fit the output row -- one that would take it past kMaxRowChannels is left out,
/// and every one after. The first is the clock.
std::vector<AudioDevice> aggregateOf(const std::vector<std::string>& chosen, const std::vector<AudioDevice>& system);
/// Their outputs, or their inputs, as a row; a device with none in that direction takes no channel of it.
Row rowOf(const std::vector<AudioDevice>& devices, bool inputs = false);

/// What the host plays out.
enum class Signal { Main, Headphones, Speakers };

/// An output played again by the host on other channels: a second pair of headphones, or back to Live.
struct Copy {
    Signal of{Signal::Headphones};
    DeviceChannel at;    ///< its first channel; unused for one back to Live
    bool toLive{false};  ///< onto the loop device's record pair, where Live's bambi track records it

    friend bool operator==(const Copy&, const Copy&) = default;
};

/// What this computer keeps of where the host plays and hears, by device channel: it wins over what a restored set's Decoder
/// holds, since a set opened elsewhere meets other devices.
struct AudioSettings {
    std::vector<std::string> devices;  ///< the chosen devices' uids, the clock first
    int buffer{64};
    DeviceChannel main;  ///< the first of Live's Main's pair
    bool headphonesOn{true};
    DeviceChannel headphones;  ///< the pair's first
    bool speakersOn{false};
    DeviceChannel speakers;  ///< where the layout's channel 1 plays
    std::string layout;      ///< a built-in's name; empty: the Decoder's own
    double levelDb{0.0};
    std::vector<Copy> copies;
    /// Where each of the field's inputs is heard: its first channel, by the input's id.
    struct InputAt {
        TrackId input{0};
        DeviceChannel at;
        friend bool operator==(const InputAt&, const InputAt&) = default;
    };
    std::vector<InputAt> inputs;

    friend bool operator==(const AudioSettings&, const AudioSettings&) = default;
};
Json toJson(const AudioSettings& s);
AudioSettings audioSettingsFrom(const Json& j);

/// The row channels a copy, or Main, takes: the pair from its first channel, or for the speakers the layout's
/// channels moved to start there. Empty for one back to Live or on no device of the row.
std::vector<int> takenBy(const Copy& c, const Row& row, const DecoderReport& decoder);
/// Every row channel the host keeps for itself: Main's pair and every copy's, ascending, each once.
std::vector<int> keptChannels(const AudioSettings& s, const Row& row, const DecoderReport& decoder);

/// What the host asks of the Decoder for these settings on this row: its outputs where they are kept, the layout,
/// the level, and the row's kept channels and devices. An output whose device is not on the row is left as the
/// Decoder has it.
DecoderRequest requestFor(const AudioSettings& s, const Row& row, const DecoderReport& decoder);

/// Where each input of the field arrives on the input row, the loop device's `loopChannels` before the input row's:
/// on the channel kept for it, all of it on one device. One whose device is not chosen does not arrive.
std::vector<Arrival> inputArrivals(const AudioSettings& s, const std::vector<HostInput>& inputs, const Row& row,
                                   int loopChannels);

/// One channel the host plays out.
struct OutputRoute {
    enum class From { Decoder, Main };
    enum class To { Row, Loop };
    From from{From::Decoder};
    int channel{0};  ///< the Decoder's bus channel, or 1 and 2 for Main's left and right
    To to{To::Row};
    int dest{0};   ///< a row channel, or the loop device's record channel, 1 or 2
    int delay{0};  ///< samples

    friend bool operator==(const OutputRoute&, const OutputRoute&) = default;
};
struct OutputPlan {
    std::vector<OutputRoute> routes;
    std::vector<int> clashes;  ///< row channels two routes reach, ascending: both are heard there
};
/// The routes for these settings on this row, with the Decoder as it reports itself and the field `fieldLatency`
/// samples late: the Decoder's outputs that play, on the same row channels; Main on its pair; every copy.
OutputPlan planOutputs(const AudioSettings& s, const Row& row, const DecoderReport& decoder, int fieldLatency);

}  // namespace bambi::live
