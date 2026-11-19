#include "proto/discovery.h"

#include <charconv>
#include <format>

namespace psvp::proto {
namespace {

// Both request datagrams end with a NUL byte after the text, as the consoles expect.
std::string with_terminator(std::string text) {
    text.push_back('\0');
    return text;
}

std::string value_of(const TextMessage& message, std::string_view name) {
    const auto found = message.find(name);
    return found ? std::string(*found) : std::string();
}

}  // namespace

std::string_view to_string(ConsoleState state) noexcept {
    switch (state) {
        case ConsoleState::ready: return "ready";
        case ConsoleState::standby: return "standby";
        case ConsoleState::unknown: break;
    }
    return "unknown";
}

bool DiscoveryReply::supported() const noexcept {
    if (kind == ConsoleKind::ps5) return true;
    long version = 0;
    const auto [end, error] =
        std::from_chars(system_version.data(), system_version.data() + system_version.size(), version);
    if (error != std::errc{} || end != system_version.data() + system_version.size()) return false;
    return version >= kOldestPs4SystemVersion;
}

std::string discovery_search_packet(ConsoleKind kind) {
    return with_terminator(std::format("SRCH * HTTP/1.1\ndevice-discovery-protocol-version:{}\n", discovery_version(kind)));
}

std::string discovery_wake_packet(ConsoleKind kind, std::uint64_t credential) {
    return with_terminator(std::format("WAKEUP * HTTP/1.1\n"
                                       "client-type:vr\n"
                                       "auth-type:R\n"
                                       "model:w\n"
                                       "app-type:r\n"
                                       "user-credential:{}\n"
                                       "device-discovery-protocol-version:{}\n",
                                       credential, discovery_version(kind)));
}

std::expected<DiscoveryReply, TextError> parse_discovery_reply(std::string_view datagram) {
    const auto message = parse_status_message(datagram);
    if (!message) return std::unexpected(message.error());

    DiscoveryReply reply;
    if (message->status == 200) reply.state = ConsoleState::ready;
    else if (message->status == 620) reply.state = ConsoleState::standby;

    reply.host_id = value_of(*message, "host-id");
    reply.host_name = value_of(*message, "host-name");
    reply.host_type = value_of(*message, "host-type");
    reply.system_version = value_of(*message, "system-version");
    reply.protocol_version = value_of(*message, "device-discovery-protocol-version");
    reply.running_app_title_id = value_of(*message, "running-app-titleid");
    reply.running_app_name = value_of(*message, "running-app-name");
    reply.kind = reply.protocol_version == discovery_version(ConsoleKind::ps5) ? ConsoleKind::ps5 : ConsoleKind::ps4;

    if (const auto port = message->find("host-request-port")) {
        unsigned value = 0;
        const auto [end, error] = std::from_chars(port->data(), port->data() + port->size(), value);
        if (error == std::errc{} && end == port->data() + port->size() && value <= 0xffff) {
            reply.request_port = static_cast<std::uint16_t>(value);
        }
    }
    return reply;
}

std::optional<std::uint64_t> wake_credential(std::string_view regist_key) {
    if (regist_key.empty() || regist_key.size() > 16) return std::nullopt;
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(regist_key.data(), regist_key.data() + regist_key.size(), value, 16);
    if (error != std::errc{} || end != regist_key.data() + regist_key.size()) return std::nullopt;
    return value;
}

}  // namespace psvp::proto
