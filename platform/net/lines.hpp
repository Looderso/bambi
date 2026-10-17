// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

/*  A local TCP server for one peer at a time that speaks in lines: what the bambi Live device and whatever stands
 *  where the host stands say to each other. It listens on 127.0.0.1 only. A second peer while one is connected is
 *  sent the busy line and closed.
 *
 *  Every peer is a connection of its own, numbered from 1, and every event names it, so what was said on one is
 *  never answered on the next. A peer that stops reading for longer than a send may wait, or sends more than
 *  kMaxLine without a newline, is dropped.
 *
 *  poll() runs on one thread -- the one that owns the server -- and calls the events there. send() may be called
 *  from any other thread, and waits at most kSendWaitMs. POSIX sockets.
 */
namespace bambi::net {

class LineServer {
public:
    using Connection = std::uint64_t;
    static constexpr std::size_t kMaxLine = 1 << 20;  ///< bytes
    static constexpr int kSendWaitMs = 50;

    struct Events {
        std::function<void(Connection)> connected;
        std::function<void(Connection, const std::string&)> line;  ///< without its newline; never an empty one
        std::function<void(Connection)> gone;
    };

    explicit LineServer(std::string busyLine) : busy_(std::move(busyLine)) {}
    ~LineServer();
    LineServer(const LineServer&) = delete;
    LineServer& operator=(const LineServer&) = delete;

    /// False when the port is taken or cannot be opened; the reason in error.
    bool listen(int port, std::string* error);

    /// Waits up to timeoutMs for a peer or for what it says, and calls the events for whatever came.
    void poll(int timeoutMs, const Events& events);

    /// Sends one line, its newline added, on `to` if it is still the peer. False when it is not, or the line could
    /// not be sent -- the peer is then dropped.
    bool send(Connection to, const std::string& line);

    bool connected() const;

private:
    void drop(const Events& events);

    std::string busy_;
    int server_{-1};
    int peer_{-1};           ///< written only on the polling thread, under mutex_
    Connection current_{0};  ///< the peer's connection; 0 for none. Likewise
    Connection numbered_{0};
    std::string pending_;
    mutable std::mutex mutex_;  ///< the peer's descriptor between poll() and send()
};

}  // namespace bambi::net
