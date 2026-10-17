// SPDX-License-Identifier: GPL-3.0-or-later
#include "lines.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace bambi::net {

namespace {

//  The whole of data, or false; a peer gone mid-line raises no signal.
bool writeAll(int fd, const std::string& data) {
    std::size_t done = 0;
    while (done < data.size()) {
#ifdef MSG_NOSIGNAL
        const ssize_t n = ::send(fd, data.data() + done, data.size() - done, MSG_NOSIGNAL);
#else
        const ssize_t n = ::send(fd, data.data() + done, data.size() - done, 0);
#endif
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

//  No signal when the peer has gone, and a send that waits at most `waitMs` for room.
void configure(int fd, int waitMs) {
#ifdef SO_NOSIGPIPE
    const int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
    timeval wait{};
    wait.tv_usec = waitMs * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &wait, sizeof wait);
}

}  // namespace

LineServer::~LineServer() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (peer_ >= 0) ::close(peer_);
    if (server_ >= 0) ::close(server_);
}

bool LineServer::listen(int port, std::string* error) {
    server_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_ < 0) {
        if (error != nullptr) *error = std::strerror(errno);
        return false;
    }
    const int on = 1;
    ::setsockopt(server_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(server_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(server_, 1) != 0) {
        if (error != nullptr) *error = std::strerror(errno);
        ::close(server_);
        server_ = -1;
        return false;
    }
    return true;
}

void LineServer::drop(const Events& events) {
    Connection was = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ::close(peer_);
        peer_ = -1;
        was = current_;
        current_ = 0;
    }
    pending_.clear();
    if (events.gone) events.gone(was);
}

void LineServer::poll(int timeoutMs, const Events& events) {
    if (server_ < 0) return;
    pollfd fds[2] = {{server_, POLLIN, 0}, {peer_, POLLIN, 0}};
    if (::poll(fds, peer_ >= 0 ? 2 : 1, timeoutMs) <= 0) return;
    if (fds[0].revents & POLLIN) {
        const int client = ::accept(server_, nullptr, nullptr);
        if (client >= 0) {
            configure(client, kSendWaitMs);
            if (peer_ >= 0) {
                (void)writeAll(client, busy_ + "\n");
                ::close(client);
            } else {
                Connection now = 0;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    peer_ = client;
                    current_ = now = ++numbered_;
                }
                pending_.clear();
                if (events.connected) events.connected(now);
            }
        }
    }
    if (peer_ >= 0 && fds[1].fd == peer_ && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
        char buf[4096];
        const ssize_t n = ::read(peer_, buf, sizeof buf);
        if (n <= 0) {
            drop(events);
            return;
        }
        pending_.append(buf, static_cast<std::size_t>(n));
        for (std::size_t at; (at = pending_.find('\n')) != std::string::npos;) {
            const std::string line = pending_.substr(0, at);
            pending_.erase(0, at + 1);
            if (!line.empty() && events.line) events.line(current_, line);
        }
        if (pending_.size() > kMaxLine) drop(events);
    }
}

bool LineServer::send(Connection to, const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (peer_ < 0 || to != current_) return false;
    if (writeAll(peer_, line + "\n")) return true;
    ::shutdown(peer_, SHUT_RDWR);  // a line half sent: the polling thread hears the peer gone and drops it
    return false;
}

bool LineServer::connected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return peer_ >= 0;
}

}  // namespace bambi::net
