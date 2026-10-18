// SPDX-License-Identifier: GPL-3.0-or-later
#include "Device.h"

#include <chrono>
#include <variant>

#include "HostLog.h"
#include "loop.hpp"

namespace bambi::livehost {

DeviceServer::DeviceServer(live::SessionSettings settings)
    : juce::Thread("bambi live device"), server_(live::serialise(live::Busy{})), session_(settings) {}

DeviceServer::~DeviceServer() {
    hostLog("host closing");
    stopThread(1000);
}

bool DeviceServer::start(std::string* error) {
    if (!server_.listen(kPort, error)) {
        hostLog("could not listen on 127.0.0.1:" + std::to_string(kPort) + ": " +
                (error != nullptr ? *error : std::string()));
        return false;
    }
    hostLog("listening on 127.0.0.1:" + std::to_string(kPort) + ", protocol " + std::to_string(live::kProtocol));
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
                                                 hostLog("a device connected");
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
        hostLog("a line the host does not understand (" + error + "): " + line.substr(0, 120));
        return;
    }
    if (const auto* hello = std::get_if<live::Hello>(&*message)) {
        hostLog("hello from Live " + hello->live + ", protocol " + std::to_string(hello->protocol) +
                (live::accepts(*hello) ? ""
                                       : ": NOT the host's protocol " + std::to_string(live::kProtocol) +
                                             " -- an old device in a saved set; it is told so and not heard"));
    } else if (const auto* tree = std::get_if<live::Tree>(&*message)) {
        if (!session_.compatible())
            hostLog("a tree of " + std::to_string(tree->nodes.size()) +
                    " nodes, ignored: the device is in another protocol");
        else if (tree->nodes.size() != lastTreeSize_)
            hostLog("the set's tree: " + std::to_string(tree->nodes.size()) + " nodes");
        lastTreeSize_ = tree->nodes.size();
    } else if (const auto* switched = std::get_if<live::Switched>(&*message)) {
        hostLog(std::string("the route button: bambi ") + (switched->on ? "on" : "off"));
    }
    const int shows = session_.shows();
    for (const live::Message& answer : session_.answer(*message)) server_.send(from, live::serialise(answer));
    const auto* value = std::get_if<live::Value>(&*message);
    if (std::holds_alternative<live::Tree>(*message) || (value != nullptr && value->what == "name"))
        (void)loop::nameChannels(session_.channelNames());
    if (const auto* t = std::get_if<live::TransportMessage>(&*message); t != nullptr && transport) transport(*t, stamp);
    if (const auto* p = std::get_if<live::SlotPlay>(&*message); p != nullptr && slot) slot(*p, stamp);
    if (std::holds_alternative<live::Restore>(*message) && restored) restored();
    if (session_.shows() != shows && show) show();
    if (changed) changed();
}

void DeviceServer::keep(std::string_view name, Json value) {
    session_.keep(name, std::move(value));
    if (device_ != 0) server_.send(device_, live::serialise(session_.save()));
}

void DeviceServer::reshape(live::Shape shape) {
    const auto answers = session_.reshape(std::move(shape));
    if (device_ != 0)
        for (const live::Message& answer : answers) server_.send(device_, live::serialise(answer));
    if (!answers.empty()) (void)loop::nameChannels(session_.channelNames());
    if (changed) changed();
}

void DeviceServer::command(const live::Command& c) {
    if (device_ != 0) server_.send(device_, live::serialise(c));
}

void DeviceServer::refresh() {
    if (device_ != 0) server_.send(device_, live::serialise(live::Refresh{}));
}

void DeviceServer::gone(net::LineServer::Connection from) {
    if (from != device_) return;
    hostLog("the device disconnected");
    device_ = 0;
    lastTreeSize_ = static_cast<std::size_t>(-1);
    session_.disconnected();
    if (changed) changed();
}

}  // namespace bambi::livehost
