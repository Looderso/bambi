// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <juce_events/juce_events.h>
#include <string>
#include <string_view>

#include "bambi/live/session.hpp"
#include "lines.hpp"

/*  The bambi Live device, as the host hears it: a line server on 127.0.0.1 on its own thread, and the session that
 *  answers on the message thread. Every line is stamped with the steady clock as it is read, then goes to the
 *  message thread to be answered, on the connection it came on; a line from a connection since gone is dropped.
 *  When the set's tracks change, the loop device's channels are named after them.
 *
 *  Create, start and destroy on the message thread; the callbacks are called there.
 */
namespace bambi::livehost {

class DeviceServer final : private juce::Thread {
public:
    static constexpr int kPort = 9461;

    explicit DeviceServer(live::SessionSettings settings);
    ~DeviceServer() override;

    /// False when the port is taken -- a second host, or another program -- with the reason in error.
    bool start(std::string* error);

    const live::DeviceSession& session() const { return session_; }
    void setLatency(double ms, double rate) { session_.setLatency(ms, rate); }

    /// A part of what the set keeps for the host, sent to the device at once when one is connected.
    void keep(std::string_view name, Json value);

    /// The user's new shape: the field laid out again over the set, its routing and the state with it sent to the
    /// device at once when one is connected, the loop device's channels named again.
    void reshape(live::Shape shape);
    /// Live's transport, asked of it; nothing while no device is connected.
    void command(const live::Command& c);
    /// The arrangement asked for again; nothing while no device is connected.
    void refresh();

    using Stamp = std::int64_t;  ///< nanoseconds of std::chrono::steady_clock, when a line was read

    std::function<void()> changed;                                        ///< something the session knows has changed
    std::function<void()> show;                                           ///< "open host" was pressed on the device
    std::function<void(const live::TransportMessage&, Stamp)> transport;  ///< Live's transport, as it came
    std::function<void(const live::SlotPlay&, Stamp)> slot;               ///< a track's session clip, as it came
    std::function<void()> restored;  ///< the set gave back what it kept: session().kept() has it

private:
    void run() override;
    void handle(net::LineServer::Connection from, const std::string& line, Stamp stamp);
    void gone(net::LineServer::Connection from);

    net::LineServer server_;
    live::DeviceSession session_;
    net::LineServer::Connection device_{0};  ///< the connection the session answers; 0 for none
    std::string lastTree_;                   ///< the tree as last logged, so the log says it again only when it changes
    juce::WeakReference<DeviceServer> self_;
    JUCE_DECLARE_WEAK_REFERENCEABLE(DeviceServer)
};

}  // namespace bambi::livehost
