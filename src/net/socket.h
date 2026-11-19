// Sockets: IPv4 UDP and TCP with deadlines. Every call that can wait takes the
// moment at which it must give up; none of them is called from the UI thread.
#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace psvp::net {

using Clock = std::chrono::steady_clock;

struct Address {
    std::uint32_t ip = 0;  // host byte order
    std::uint16_t port = 0;
    friend bool operator==(const Address&, const Address&) = default;
};

inline constexpr std::uint32_t kAnyIp = 0;
inline constexpr std::uint32_t kLoopbackIp = 0x7f000001;
inline constexpr std::uint32_t kBroadcastIp = 0xffffffff;

[[nodiscard]] std::string ip_to_string(std::uint32_t ip);
[[nodiscard]] std::string to_string(const Address& address);
[[nodiscard]] std::optional<std::uint32_t> parse_ipv4(std::string_view text);

// A dotted address or a host name. Looking a name up can take seconds.
[[nodiscard]] std::expected<std::uint32_t, std::string> resolve_ipv4(const std::string& host);

// The directed broadcast address of every network interface that is up and has one.
[[nodiscard]] std::vector<std::uint32_t> interface_broadcasts();

// Owns a file descriptor.
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) noexcept : fd_(fd) {}
    Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd& operator=(Fd&& other) noexcept;
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd() { reset(); }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    void reset() noexcept;

private:
    int fd_ = -1;
};

struct Datagram {
    Address from;
    std::vector<std::uint8_t> data;
};

class UdpSocket {
public:
    // Bound to `bind_ip` on the first free port of [first_port, last_port];
    // 0 and 0 let the system choose one.
    [[nodiscard]] static std::expected<UdpSocket, std::string> open(std::uint32_t bind_ip, std::uint16_t first_port,
                                                                   std::uint16_t last_port, bool allow_broadcast);

    [[nodiscard]] std::expected<void, std::string> send_to(const Address& to, std::span<const std::uint8_t> data);
    // Nothing when no datagram arrived before the deadline.
    [[nodiscard]] std::expected<std::optional<Datagram>, std::string> receive(Clock::time_point deadline);
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] int fd() const noexcept { return fd_.get(); }

private:
    Fd fd_;
    std::uint16_t port_ = 0;
};

class TcpStream {
public:
    TcpStream() = default;
    explicit TcpStream(Fd fd) noexcept : fd_(std::move(fd)) {}

    [[nodiscard]] static std::expected<TcpStream, std::string> connect(const Address& to, Clock::time_point deadline);

    [[nodiscard]] std::expected<void, std::string> send_all(std::span<const std::uint8_t> data, Clock::time_point deadline);
    // Up to buffer.size() bytes; 0 when the other side has closed.
    [[nodiscard]] std::expected<std::size_t, std::string> receive(std::span<std::uint8_t> buffer, Clock::time_point deadline);
    [[nodiscard]] int fd() const noexcept { return fd_.get(); }
    [[nodiscard]] bool open() const noexcept { return fd_.valid(); }
    void close() noexcept { fd_.reset(); }

private:
    Fd fd_;
};

// What was read of a text head (a request or a response up to its blank line),
// and the bytes that had already arrived after it.
struct TextHead {
    std::string head;
    std::vector<std::uint8_t> rest;
};

// Reads until the blank line that ends a head of at most `max_size` bytes.
[[nodiscard]] std::expected<TextHead, std::string> read_text_head(TcpStream& stream, Clock::time_point deadline,
                                                                 std::size_t max_size);

struct Accepted {
    TcpStream stream;
    Address from;
};

class TcpListener {
public:
    [[nodiscard]] static std::expected<TcpListener, std::string> open(std::uint32_t bind_ip, std::uint16_t port);
    // Nothing when nobody connected before the deadline.
    [[nodiscard]] std::expected<std::optional<Accepted>, std::string> accept(Clock::time_point deadline);
    [[nodiscard]] int fd() const noexcept { return fd_.get(); }

private:
    Fd fd_;
};

// Helpers for byte views of text.
[[nodiscard]] inline std::span<const std::uint8_t> bytes_of(std::string_view text) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

}  // namespace psvp::net
