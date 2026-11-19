// A stand-in for a console, for tests: it answers discovery, takes a wake-up
// and accepts a registration the way the protocol pages describe a console
// doing it. It is not a console and cannot stream anything.
//
//   psvp-mock-console [options]
//     --kind ps5|ps4          which family to stand in for (default ps5)
//     --name NAME             the console's name (default "Mock Console")
//     --state ready|standby   on, or in rest mode (default ready)
//     --pin NUMBER            the eight-digit PIN it "shows"; without one it takes no registration
//     --account-id BASE64     only this account may register (default: any)
//     --regist-key KEY        the registration key it hands out and wakes for (default 1a2b3c4d)
//     --system-version TEXT   as discovery reports it (default 09000004 or 12020000)
//     --bind ADDRESS          where it listens (default 127.0.0.1; 0.0.0.0 to hear broadcasts)
//     --dump DIR              keep every registration request it receives in DIR
//     --ready-file FILE       created once it is listening
//
// It listens on the consoles' real ports, so it needs a network of its own
// (tools/wall.sh, or a container) and the right to open port 987 for a PS4.
// One line per event goes to standard output.
#include <algorithm>
#include <array>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <poll.h>

#include "net/socket.h"
#include "proto/base64.h"
#include "proto/discovery.h"
#include "proto/hex.h"
#include "proto/regist.h"
#include "proto/rp_cipher.h"
#include "proto/text_message.h"

namespace {

using namespace std::chrono_literals;
using psvp::net::Clock;
using psvp::proto::ConsoleKind;
using psvp::proto::ConsoleState;

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

template <class... Args>
void event(std::format_string<Args...> format, Args&&... args) {
    std::fputs(("mock: " + std::format(format, std::forward<Args>(args)...) + "\n").c_str(), stdout);
    std::fflush(stdout);
}

struct Options {
    ConsoleKind kind = ConsoleKind::ps5;
    std::string name = "Mock Console";
    ConsoleState state = ConsoleState::ready;
    std::optional<std::uint32_t> pin;
    std::string account_id;
    std::string regist_key = "1a2b3c4d";
    std::string system_version;
    std::uint32_t bind_ip = psvp::net::kLoopbackIp;
    std::string dump_dir;
    std::string ready_file;
};

constexpr std::array<std::uint8_t, 6> kMac{0x02, 0x00, 0x5e, 0x10, 0x20, 0x30};
constexpr psvp::proto::Key16 kSecret{0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf};
// The reason a console gives when a registration fails.
constexpr std::string_view kReasonRegistFailed = "80108b09";

std::string upper_hex(std::span<const std::uint8_t> bytes) {
    std::string text = psvp::proto::hex_encode(bytes);
    std::ranges::transform(text, text.begin(), [](char c) { return c >= 'a' && c <= 'f' ? static_cast<char>(c - 'a' + 'A') : c; });
    return text;
}

std::string discovery_reply(const Options& options, ConsoleState state) {
    const std::string family(psvp::proto::to_string(options.kind));
    std::string text = state == ConsoleState::ready ? "HTTP/1.1 200 Ok\n" : "HTTP/1.1 620 Server Standby\n";
    text += std::format("host-id:{}\nhost-type:{}\nhost-name:{}\nhost-request-port:997\n"
                        "device-discovery-protocol-version:{}\nsystem-version:{}\n",
                        upper_hex(kMac), family, options.name, psvp::proto::discovery_version(options.kind), options.system_version);
    return text;
}

std::string refusal() {
    return std::format("HTTP/1.1 403 Forbidden\r\nRP-Application-Reason: {}\r\nContent-Length: 0\r\n\r\n", kReasonRegistFailed);
}

// Reads one registration request and answers it. Returns the line to log.
std::string serve_registration(const Options& options, psvp::net::TcpStream& stream, int number) {
    using namespace psvp::proto::regist_layout;
    const auto deadline = Clock::now() + 3s;
    std::vector<std::uint8_t> data;
    std::size_t head_size = 0;
    std::size_t body_size = 0;
    while (true) {
        const std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
        if (head_size == 0) {
            const std::size_t end = text.find("\r\n\r\n");
            if (end != std::string_view::npos) {
                head_size = end + 4;
                const std::size_t field = text.find("Content-Length: ");
                if (field == std::string_view::npos || field > end) return "registration without a length";
                const char* begin = text.data() + field + 16;
                std::from_chars(begin, text.data() + end, body_size);
                if (body_size == 0 || body_size > 4096) return "registration with a bad length";
            }
        }
        if (head_size != 0 && data.size() >= head_size + body_size) break;
        std::uint8_t chunk[1024];
        const auto got = stream.receive(chunk, deadline);
        if (!got || *got == 0) return "registration cut short";
        data.insert(data.end(), chunk, chunk + *got);
        if (data.size() > 8192) return "registration too long";
    }
    const std::string head(reinterpret_cast<const char*>(data.data()), head_size);
    const std::span<const std::uint8_t> payload(data.data() + head_size, body_size);
    if (!options.dump_dir.empty()) {
        std::ofstream(std::format("{}/regist-{}.head", options.dump_dir, number), std::ios::binary) << head;
        std::ofstream(std::format("{}/regist-{}.payload", options.dump_dir, number), std::ios::binary)
            .write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    }

    const auto answer = [&](const std::string& response) {
        (void)stream.send_all(psvp::net::bytes_of(response), Clock::now() + 3s);
    };
    const std::string path = options.kind == ConsoleKind::ps5 ? "POST /sie/ps5/rp/sess/rgst " : "POST /sie/ps4/rp/sess/rgst ";
    if (!head.starts_with(path)) {
        answer(refusal());
        return "registration refused: not this console's request path";
    }
    if (!options.pin || payload.size() <= kInnerOffset) {
        answer(refusal());
        return "registration refused: no PIN is being shown";
    }

    // The request says which table entries it used and carries the wrapped nonce.
    const std::size_t key_offset = payload[kKeyOffsetByte] & 0x1f;
    const std::size_t wrap_offset = payload[kWrapOffsetByte] >> 3;
    psvp::proto::Key16 wrapped{};
    std::copy_n(payload.begin() + kWrappedLowAt, 8, wrapped.begin());
    std::copy_n(payload.begin() + kWrappedHighAt, 8, wrapped.begin() + 8);
    const auto nonce = psvp::proto::unwrap_nonce(options.kind, wrapped, wrap_offset);
    const auto cipher = nonce ? psvp::proto::make_regist_cipher(options.kind, *nonce, key_offset, *options.pin) : std::nullopt;
    if (!cipher) {
        answer(refusal());
        return "registration refused: bad table offsets";
    }
    const std::vector<std::uint8_t> opened = psvp::proto::decrypt(*cipher, 0, payload.subspan(kInnerOffset));
    const auto inner = psvp::proto::parse_header_block(std::string_view(reinterpret_cast<const char*>(opened.data()), opened.size()));
    const auto account = inner ? inner->find("Np-AccountId") : std::nullopt;
    if (!inner || !inner->find("Client-Type") || !account) {
        // With the wrong PIN the text does not open to anything readable.
        answer(refusal());
        return "registration refused: wrong PIN";
    }
    if (!options.account_id.empty() && *account != options.account_id) {
        answer(refusal());
        return std::format("registration refused: account {} is not this console's", *account);
    }

    const std::string family(psvp::proto::to_string(options.kind));
    const std::string body = std::format("AP-Ssid: {0}-MOCK\r\n"
                                         "AP-Bssid: {1}\r\n"
                                         "AP-Key: mockapkey\r\n"
                                         "AP-Name: {0}\r\n"
                                         "{0}-Mac: {1}\r\n"
                                         "{0}-RegistKey: {2}\r\n"
                                         "{0}-Nickname: {3}\r\n"
                                         "RP-KeyType: 2\r\n"
                                         "RP-Key: {4}\r\n",
                                         family, psvp::proto::hex_encode(kMac),
                                         psvp::proto::hex_encode(psvp::net::bytes_of(options.regist_key)), options.name,
                                         psvp::proto::hex_encode(kSecret));
    const std::vector<std::uint8_t> sealed = psvp::proto::encrypt(*cipher, 0, psvp::net::bytes_of(body));
    std::string response = std::format("HTTP/1.1 200 OK\r\nContent-Length: {}\r\n\r\n", sealed.size());
    response.append(reinterpret_cast<const char*>(sealed.data()), sealed.size());
    answer(response);
    return std::format("registration accepted: account {} key-offset {} wrap-offset {}", *account, key_offset, wrap_offset);
}

int usage(std::string_view problem) {
    std::fputs(std::format("psvp-mock-console: {}\n", problem).c_str(), stderr);
    return 64;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto value = [&]() -> std::optional<std::string_view> {
            if (i + 1 >= argc) return std::nullopt;
            return std::string_view(argv[++i]);
        };
        const auto text = value();
        if (!text) return usage(std::format("{} needs a value", arg));
        if (arg == "--kind") {
            if (*text == "ps5") options.kind = ConsoleKind::ps5;
            else if (*text == "ps4") options.kind = ConsoleKind::ps4;
            else return usage("--kind is ps5 or ps4");
        } else if (arg == "--name") {
            options.name = *text;
        } else if (arg == "--state") {
            if (*text == "ready") options.state = ConsoleState::ready;
            else if (*text == "standby") options.state = ConsoleState::standby;
            else return usage("--state is ready or standby");
        } else if (arg == "--pin") {
            std::uint32_t pin = 0;
            const auto [end, error] = std::from_chars(text->data(), text->data() + text->size(), pin);
            if (error != std::errc{} || end != text->data() + text->size()) return usage("--pin is a number");
            options.pin = pin;
        } else if (arg == "--account-id") {
            options.account_id = *text;
        } else if (arg == "--regist-key") {
            options.regist_key = *text;
        } else if (arg == "--system-version") {
            options.system_version = *text;
        } else if (arg == "--bind") {
            const auto ip = psvp::net::parse_ipv4(*text);
            if (!ip) return usage("--bind is an IPv4 address");
            options.bind_ip = *ip;
        } else if (arg == "--dump") {
            options.dump_dir = *text;
        } else if (arg == "--ready-file") {
            options.ready_file = *text;
        } else {
            return usage(std::format("unknown option {}", arg));
        }
    }
    if (options.system_version.empty()) options.system_version = options.kind == ConsoleKind::ps5 ? "09000004" : "12020000";

    std::signal(SIGTERM, on_signal);
    std::signal(SIGINT, on_signal);

    const std::uint16_t port = psvp::proto::discovery_port(options.kind);
    auto discovery = psvp::net::UdpSocket::open(options.bind_ip, port, port, true);
    auto search = psvp::net::UdpSocket::open(options.bind_ip, psvp::proto::kSessionPort, psvp::proto::kSessionPort, true);
    auto listener = psvp::net::TcpListener::open(options.bind_ip, psvp::proto::kSessionPort);
    if (!discovery) return usage(std::format("discovery port {}: {}", port, discovery.error()));
    if (!search) return usage(std::format("session port: {}", search.error()));
    if (!listener) return usage(std::format("session port: {}", listener.error()));

    ConsoleState state = options.state;
    int registrations = 0;
    event("listening as {} '{}' ({}) on {}, ports {} and {}", psvp::proto::to_string(options.kind), options.name,
          psvp::proto::to_string(state), psvp::net::ip_to_string(options.bind_ip), port, psvp::proto::kSessionPort);
    if (!options.ready_file.empty()) std::ofstream(options.ready_file) << "ready\n";

    while (g_stop == 0) {
        pollfd waits[3] = {{discovery->fd(), POLLIN, 0}, {search->fd(), POLLIN, 0}, {listener->fd(), POLLIN, 0}};
        if (::poll(waits, 3, 200) <= 0) continue;

        if ((waits[0].revents & POLLIN) != 0) {
            const auto datagram = discovery->receive(Clock::now());
            if (datagram && *datagram) {
                const std::string_view text(reinterpret_cast<const char*>((*datagram)->data.data()), (*datagram)->data.size());
                const bool for_us = text.find(psvp::proto::discovery_version(options.kind)) != std::string_view::npos;
                if (text.starts_with("SRCH * HTTP/1.1") && for_us) {
                    (void)discovery->send_to((*datagram)->from, psvp::net::bytes_of(discovery_reply(options, state)));
                    event("search answered as {}", psvp::proto::to_string(state));
                } else if (text.starts_with("WAKEUP * HTTP/1.1") && for_us) {
                    const std::size_t field = text.find("user-credential:");
                    std::uint64_t credential = 0;
                    bool have = false;
                    if (field != std::string_view::npos) {
                        const char* begin = text.data() + field + 16;
                        const char* end = text.data() + std::min(text.find('\n', field), text.size());
                        have = std::from_chars(begin, end, credential).ec == std::errc{};
                    }
                    if (have && credential == psvp::proto::wake_credential(options.regist_key)) {
                        if (state != ConsoleState::ready) event("wake-up accepted, now on");
                        state = ConsoleState::ready;
                    } else {
                        event("wake-up refused: credential {} is not a registered client's", credential);
                    }
                } else {
                    event("datagram ignored on the discovery port ({} bytes)", text.size());
                }
            }
        }
        if ((waits[1].revents & POLLIN) != 0) {
            const auto datagram = search->receive(Clock::now());
            if (datagram && *datagram) {
                const std::string wanted = psvp::proto::regist_search_packet(options.kind);
                const std::string_view text(reinterpret_cast<const char*>((*datagram)->data.data()), (*datagram)->data.size());
                if (text == wanted && options.pin && state == ConsoleState::ready) {
                    const std::string reply = options.kind == ConsoleKind::ps5 ? "RES3" : "RES2";
                    (void)search->send_to((*datagram)->from, psvp::net::bytes_of(reply));
                    event("registration search answered");
                } else {
                    event("registration search ignored");
                }
            }
        }
        if ((waits[2].revents & POLLIN) != 0) {
            auto accepted = listener->accept(Clock::now());
            if (accepted && *accepted) {
                const std::string line = serve_registration(options, (*accepted)->stream, ++registrations);
                event("{}", line);
            }
        }
    }
    event("stopped");
    return 0;
}
