#include "net/socket.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <format>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace psvp::net {
namespace {

std::string error_text(std::string_view what) { return std::format("{}: {}", what, std::strerror(errno)); }

sockaddr_in to_sockaddr(const Address& address) {
    sockaddr_in out{};
    out.sin_family = AF_INET;
    out.sin_port = htons(address.port);
    out.sin_addr.s_addr = htonl(address.ip);
    return out;
}

Address from_sockaddr(const sockaddr_in& in) { return Address{ntohl(in.sin_addr.s_addr), ntohs(in.sin_port)}; }

// Waits for `events` on the descriptor. True when ready, false at the deadline.
std::expected<bool, std::string> wait_for(int fd, short events, Clock::time_point deadline) {
    while (true) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (left <= 0) return false;
        pollfd entry{fd, events, 0};
        const int ready = ::poll(&entry, 1, static_cast<int>(std::min<long long>(left, 60'000)));
        if (ready > 0) return true;
        if (ready < 0 && errno != EINTR) return std::unexpected(error_text("poll"));
    }
}

}  // namespace

std::string ip_to_string(std::uint32_t ip) {
    return std::format("{}.{}.{}.{}", (ip >> 24) & 0xff, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff);
}

std::string to_string(const Address& address) { return std::format("{}:{}", ip_to_string(address.ip), address.port); }

std::optional<std::uint32_t> parse_ipv4(std::string_view text) {
    std::uint32_t ip = 0;
    int parts = 0;
    std::size_t at = 0;
    while (parts < 4) {
        std::uint32_t value = 0;
        std::size_t digits = 0;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9' && digits < 3) {
            value = value * 10 + static_cast<std::uint32_t>(text[at] - '0');
            ++at;
            ++digits;
        }
        if (digits == 0 || value > 255) return std::nullopt;
        ip = (ip << 8) | value;
        ++parts;
        if (parts < 4) {
            if (at >= text.size() || text[at] != '.') return std::nullopt;
            ++at;
        }
    }
    if (at != text.size()) return std::nullopt;
    return ip;
}

std::expected<std::uint32_t, std::string> resolve_ipv4(const std::string& host) {
    if (const auto literal = parse_ipv4(host)) return *literal;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* found = nullptr;
    const int result = ::getaddrinfo(host.c_str(), nullptr, &hints, &found);
    if (result != 0 || found == nullptr) {
        return std::unexpected(std::format("no address found for '{}': {}", host, ::gai_strerror(result)));
    }
    sockaddr_in first{};
    std::memcpy(&first, found->ai_addr, std::min<std::size_t>(sizeof(first), found->ai_addrlen));
    ::freeaddrinfo(found);
    return ntohl(first.sin_addr.s_addr);
}

std::vector<std::uint32_t> interface_broadcasts() {
    std::vector<std::uint32_t> out;
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) return out;
    for (const ifaddrs* entry = list; entry != nullptr; entry = entry->ifa_next) {
        if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET) continue;
        if ((entry->ifa_flags & IFF_UP) == 0 || (entry->ifa_flags & IFF_BROADCAST) == 0) continue;
        if (entry->ifa_broadaddr == nullptr) continue;
        sockaddr_in broadcast{};
        std::memcpy(&broadcast, entry->ifa_broadaddr, sizeof(broadcast));
        const std::uint32_t ip = ntohl(broadcast.sin_addr.s_addr);
        if (ip != 0 && std::ranges::find(out, ip) == out.end()) out.push_back(ip);
    }
    ::freeifaddrs(list);
    return out;
}

Fd& Fd::operator=(Fd&& other) noexcept {
    if (this != &other) {
        reset();
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

void Fd::reset() noexcept {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

std::expected<UdpSocket, std::string> UdpSocket::open(std::uint32_t bind_ip, std::uint16_t first_port,
                                                     std::uint16_t last_port, bool allow_broadcast) {
    UdpSocket socket;
    socket.fd_ = Fd(::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_UDP));
    if (!socket.fd_.valid()) return std::unexpected(error_text("socket"));
    if (allow_broadcast) {
        const int on = 1;
        if (::setsockopt(socket.fd_.get(), SOL_SOCKET, SO_BROADCAST, &on, sizeof(on)) != 0) {
            return std::unexpected(error_text("setsockopt SO_BROADCAST"));
        }
    }
    bool bound = false;
    for (std::uint32_t port = first_port; port <= last_port; ++port) {
        const sockaddr_in address = to_sockaddr(Address{bind_ip, static_cast<std::uint16_t>(port)});
        if (::bind(socket.fd_.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
            bound = true;
            break;
        }
        if (errno != EADDRINUSE) return std::unexpected(error_text(std::format("bind to port {}", port)));
    }
    if (!bound) {
        return std::unexpected(std::format("no free port between {} and {}", first_port, last_port));
    }
    sockaddr_in actual{};
    socklen_t actual_size = sizeof(actual);
    if (::getsockname(socket.fd_.get(), reinterpret_cast<sockaddr*>(&actual), &actual_size) != 0) {
        return std::unexpected(error_text("getsockname"));
    }
    socket.port_ = ntohs(actual.sin_port);
    return socket;
}

std::expected<void, std::string> UdpSocket::send_to(const Address& to, std::span<const std::uint8_t> data) {
    const sockaddr_in address = to_sockaddr(to);
    const ssize_t sent =
        ::sendto(fd_.get(), data.data(), data.size(), MSG_NOSIGNAL, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    if (sent < 0) return std::unexpected(error_text(std::format("send to {}", to_string(to))));
    return {};
}

std::expected<std::optional<Datagram>, std::string> UdpSocket::receive(Clock::time_point deadline) {
    // Reads first and waits only when nothing is there, so a datagram that has
    // already arrived is returned even when the deadline is now.
    while (true) {
        Datagram datagram;
        datagram.data.resize(2048);
        sockaddr_in from{};
        socklen_t from_size = sizeof(from);
        const ssize_t got = ::recvfrom(fd_.get(), datagram.data.data(), datagram.data.size(), 0,
                                       reinterpret_cast<sockaddr*>(&from), &from_size);
        if (got >= 0) {
            datagram.data.resize(static_cast<std::size_t>(got));
            datagram.from = from_sockaddr(from);
            return std::optional<Datagram>(std::move(datagram));
        }
        // A refused earlier datagram is reported on a later read; it is not this read's failure.
        if (errno == EINTR || errno == ECONNREFUSED) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return std::unexpected(error_text("receive"));
        const auto ready = wait_for(fd_.get(), POLLIN, deadline);
        if (!ready) return std::unexpected(ready.error());
        if (!*ready) return std::optional<Datagram>();
    }
}

std::expected<TcpStream, std::string> TcpStream::connect(const Address& to, Clock::time_point deadline) {
    Fd fd(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP));
    if (!fd.valid()) return std::unexpected(error_text("socket"));
    const sockaddr_in address = to_sockaddr(to);
    if (::connect(fd.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        if (errno != EINPROGRESS) return std::unexpected(error_text(std::format("connect to {}", to_string(to))));
        const auto ready = wait_for(fd.get(), POLLOUT, deadline);
        if (!ready) return std::unexpected(ready.error());
        if (!*ready) return std::unexpected(std::format("connect to {}: timed out", to_string(to)));
        int failure = 0;
        socklen_t failure_size = sizeof(failure);
        if (::getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, &failure, &failure_size) != 0 || failure != 0) {
            return std::unexpected(std::format("connect to {}: {}", to_string(to), std::strerror(failure != 0 ? failure : errno)));
        }
    }
    return TcpStream(std::move(fd));
}

std::expected<void, std::string> TcpStream::send_all(std::span<const std::uint8_t> data, Clock::time_point deadline) {
    while (!data.empty()) {
        const ssize_t sent = ::send(fd_.get(), data.data(), data.size(), MSG_NOSIGNAL);
        if (sent > 0) {
            data = data.subspan(static_cast<std::size_t>(sent));
            continue;
        }
        if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return std::unexpected(error_text("send"));
        const auto ready = wait_for(fd_.get(), POLLOUT, deadline);
        if (!ready) return std::unexpected(ready.error());
        if (!*ready) return std::unexpected(std::string("send: timed out"));
    }
    return {};
}

std::expected<std::size_t, std::string> TcpStream::receive(std::span<std::uint8_t> buffer, Clock::time_point deadline) {
    while (true) {
        const ssize_t got = ::recv(fd_.get(), buffer.data(), buffer.size(), 0);
        if (got >= 0) return static_cast<std::size_t>(got);
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return std::unexpected(error_text("receive"));
        const auto ready = wait_for(fd_.get(), POLLIN, deadline);
        if (!ready) return std::unexpected(ready.error());
        if (!*ready) return std::unexpected(std::string("receive: timed out"));
    }
}

std::expected<TcpListener, std::string> TcpListener::open(std::uint32_t bind_ip, std::uint16_t port) {
    TcpListener listener;
    listener.fd_ = Fd(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP));
    if (!listener.fd_.valid()) return std::unexpected(error_text("socket"));
    const int on = 1;
    ::setsockopt(listener.fd_.get(), SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    const sockaddr_in address = to_sockaddr(Address{bind_ip, port});
    if (::bind(listener.fd_.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        return std::unexpected(error_text(std::format("bind to port {}", port)));
    }
    if (::listen(listener.fd_.get(), 8) != 0) return std::unexpected(error_text("listen"));
    return listener;
}

std::expected<std::optional<Accepted>, std::string> TcpListener::accept(Clock::time_point deadline) {
    while (true) {
        sockaddr_in from{};
        socklen_t from_size = sizeof(from);
        const int fd = ::accept4(fd_.get(), reinterpret_cast<sockaddr*>(&from), &from_size, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd >= 0) return std::optional<Accepted>(Accepted{TcpStream(Fd(fd)), from_sockaddr(from)});
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR && errno != ECONNABORTED) {
            return std::unexpected(error_text("accept"));
        }
        const auto ready = wait_for(fd_.get(), POLLIN, deadline);
        if (!ready) return std::unexpected(ready.error());
        if (!*ready) return std::optional<Accepted>();
    }
}

}  // namespace psvp::net
