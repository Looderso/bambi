// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

#include "bambi/decode/remote.hpp"
#include "bambi/live/field.hpp"
#include "bambi/patch/json.hpp"

/*  Where the live host plays everything out, and hears its own inputs. The devices the user chose lie side by side as one row of output
 *  channels, the clock's first. Each Decoder places its outputs on its own bus; the host plays each, as a block,
 *  onto the row where this computer's settings put it, and Live's Main and every copy of an output too.
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

/// The most channels the row holds, as a VST3 bus does.
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
};
/// The devices the host joins with the loop device: those `chosen`, in order, that the system has and offers, each
/// once, while every one's outputs fit the output row -- one that would take it past kMaxRowChannels is left out,
/// and every one after. The first is the clock.
std::vector<AudioDevice> aggregateOf(const std::vector<std::string>& chosen, const std::vector<AudioDevice>& system);
/// Their outputs, or their inputs, as a row; a device with none in that direction takes no channel of it.
Row rowOf(const std::vector<AudioDevice>& devices, bool inputs = false);

/// What the host plays out: Live's Main, or one of a Decoder's outputs.
struct Out {
    std::uint64_t decoder{0};          ///< the Decoder's id; 0 for Live's Main
    Output which{Output::Headphones};  ///< a Decoder's output; nothing for Main
    bool main() const { return decoder == 0; }
    friend bool operator==(const Out&, const Out&) = default;
};
inline constexpr Out kLiveMain{};

/// A Decoder as it plays now: its id, where it reports its outputs on its own bus, and its latency.
struct PlayingDecoder {
    std::uint64_t id{0};
    DecoderReport report;
    int latency{0};
};

/// An output played again by the host on other channels: a second pair of headphones, or back to Live.
struct Copy {
    Out of;
    DeviceChannel at;    ///< its first channel; unused for one back to Live
    bool toLive{false};  ///< onto the loop device's record pair, where Live's bambi track records it

    friend bool operator==(const Copy&, const Copy&) = default;
};

/// Where a Decoder's output plays: its first channel, the rest after it as they lie on the Decoder's bus.
struct Placed {
    Out out;
    DeviceChannel at;
    friend bool operator==(const Placed&, const Placed&) = default;
};

/// What this computer keeps of where the host plays and hears, by device channel: a set opened elsewhere meets
/// other devices. What a Decoder plays, and how, is the Decoder's and travels with the set.
/// The longest Live's MIDI is held back.
constexpr double kMaxLiveMidiMs = 250.0;

struct AudioSettings {
    std::vector<std::string> devices;  ///< the chosen devices' uids, the clock first
    int buffer{64};
    DeviceChannel main;          ///< the first of Live's Main's pair
    std::vector<Placed> placed;  ///< each Decoder output's place; one placed nowhere is not heard
    std::vector<Copy> copies;
    /// Where each of the field's inputs is heard: its first channel, by the input's id.
    struct InputAt {
        TrackId input{0};
        DeviceChannel at;
        friend bool operator==(const InputAt&, const InputAt&) = default;
    };
    std::vector<InputAt> inputs;
    /// How long Live's MIDI, through the host's own port, is held back to meet Live's audio: how far it arrives ahead.
    double liveMidiMs{0.0};

    /// Where `out` is placed: Main's, or a Decoder output's; none when it is placed nowhere.
    DeviceChannel at(const Out& out) const;
    friend bool operator==(const AudioSettings&, const AudioSettings&) = default;
};
Json toJson(const AudioSettings& s);
AudioSettings audioSettingsFrom(const Json& j);

/// The channels `out` plays on its Decoder's bus, as the Decoder reports them: the headphones' pair, the layout's
/// channels; none while that output is off, or for a Decoder not among `decoders`. Main's are its own two.
std::vector<int> busChannels(const Out& out, const std::vector<PlayingDecoder>& decoders);
/// The row channels `out` takes from `at`: its bus channels moved as a block to start there, those past the row
/// left out. Empty on no device of the row.
std::vector<int> takenBy(const Out& out, const DeviceChannel& at, const Row& row,
                         const std::vector<PlayingDecoder>& decoders);

/// Where each input of the field arrives on the input row, the loop device's `loopChannels` before the input row's:
/// on the channel kept for it, all of it on one device. One whose device is not chosen does not arrive.
std::vector<Arrival> inputArrivals(const AudioSettings& s, const std::vector<HostInput>& inputs, const Row& row,
                                   int loopChannels);

/// One channel the host plays out.
struct OutputRoute {
    enum class From { Decoder, Main };
    enum class To { Row, Loop };
    From from{From::Decoder};
    int decoder{0};  ///< which Decoder, by its place among those given
    int channel{0};  ///< that Decoder's bus channel, or 1 and 2 for Main's left and right
    To to{To::Row};
    int dest{0};   ///< a row channel, or the loop device's record channel, 1 or 2
    int delay{0};  ///< samples

    friend bool operator==(const OutputRoute&, const OutputRoute&) = default;
};
struct OutputPlan {
    std::vector<OutputRoute> routes;
    std::vector<int> clashes;  ///< row channels two routes reach, ascending: both are heard there
};
/// The routes for these settings on this row, with the Decoders as they report themselves and the field
/// `fieldLatency` samples late through the slowest of them: each Decoder output that plays and is placed, onto its
/// place, a quicker Decoder's held back to the slowest; Main on its pair; every copy.
OutputPlan planOutputs(const AudioSettings& s, const Row& row, const std::vector<PlayingDecoder>& decoders,
                       int fieldLatency);

}  // namespace bambi::live
