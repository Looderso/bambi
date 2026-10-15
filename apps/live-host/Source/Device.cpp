// SPDX-License-Identifier: GPL-3.0-or-later
#include "Device.h"

#include <chrono>
#include <variant>

#include "loop.hpp"

namespace bambi::livehost {

DeviceServer::DeviceServer(live::SessionSettings settings)
    : juce::Thread("bambi live device"), server_(live::serialise(live::Busy{})), session_(settings) {}

DeviceServer::~DeviceServer() { stopThread(1000); }

bool DeviceServer::start(std::string* error) {
    if (!server_.listen(kPort, error)) return false;
    self_ = this;
    startThread();
    return true;
}

void DeviceServer::run() {
    //  Each event goes to the message thread; one arriving after the server is gone is dropped there.
    const juce::WeakReference<DeviceServer> self = self_;
    using Connection = net::LineServer::Connection;
    const net::LineServer::Events events{[self](Connection c) {
                                             juce::MessageManager::callAsync([self, c] {
                                                 if (self == nullptr) return;
                                                 self->device_ = c;
                                                 if (self->changed) self->changed();
                                             });
                                         },
                                         [self](Connection c, const std::string& line) {
                                             const Stamp stamp =
                                                 std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                     std::chrono::steady_clock::now().time_since_epoch())
                                                     .count();
                                             juce::MessageManager::callAsync([self, c, line, stamp] {
                                                 if (self != nullptr) self->handle(c, line, stamp);
                                             });
                                         },
                                         [self](Connection c) {
                                             juce::MessageManager::callAsync([self, c] {
                                                 if (self != nullptr) self->gone(c);
                                             });
                                         }};
    while (!threadShouldExit()) server_.poll(100, events);
}

void DeviceServer::handle(net::LineServer::Connection from, const std::string& line, Stamp stamp) {
    if (from != device_) return;
    std::string error;
    const auto message = live::parse(line, &error);
    if (!message) {
        DBG("bambi live host: not understood (" << error << "): " << line);
        return;
    }
    const int shows = session_.shows();
    for (const live::Message& answer : session_.answer(*message)) server_.send(from, live::serialise(answer));
    const auto* value = std::get_if<live::Value>(&*message);
    if (std::holds_alternative<live::Tree>(*message) || (value != nullptr && value->what == "name"))
        (void)loop::nameChannels(session_.channelNames());
    if (const auto* t = std::get_if<live::TransportMessage>(&*message); t != nullptr && transport) transport(*t, stamp);
    if (session_.shows() != shows && show) show();
    if (changed) changed();
}

void DeviceServer::gone(net::LineServer::Connection from) {
    if (from != device_) return;
    device_ = 0;
    session_.disconnected();
    if (changed) changed();
}

}  // namespace bambi::livehost
