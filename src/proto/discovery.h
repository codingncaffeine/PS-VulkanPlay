// Finding consoles on the network and waking them from rest mode.
// docs/protocol/discovery.md describes the exchange.
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include "proto/console.h"
#include "proto/text_message.h"

namespace psvp::proto {

enum class ConsoleState { unknown, ready, standby };

[[nodiscard]] std::string_view to_string(ConsoleState state) noexcept;

// What a console says about itself when asked.
struct DiscoveryReply {
    ConsoleState state = ConsoleState::unknown;
    ConsoleKind kind = ConsoleKind::ps4;
    std::string host_id;        // twelve hex digits, the console's network address
    std::string host_name;      // the name its owner gave it
    std::string host_type;
    std::string system_version; // eight digits, e.g. 08050001
    std::string protocol_version;
    std::string running_app_title_id;
    std::string running_app_name;
    std::uint16_t request_port = 0;

    // False for a PS4 whose system software predates the protocol this client speaks.
    [[nodiscard]] bool supported() const noexcept;
};

// The datagram that asks consoles of one family to identify themselves. Sent to
// discovery_port(kind), to one address or to a broadcast address.
[[nodiscard]] std::string discovery_search_packet(ConsoleKind kind);

// The datagram that wakes a console from rest mode. The credential comes from
// the registration with that console (wake_credential).
[[nodiscard]] std::string discovery_wake_packet(ConsoleKind kind, std::uint64_t credential);

[[nodiscard]] std::expected<DiscoveryReply, TextError> parse_discovery_reply(std::string_view datagram);

// A registration key is up to sixteen hex digits; read as a number it is the
// wake credential. Nothing is returned for any other text.
[[nodiscard]] std::optional<std::uint64_t> wake_credential(std::string_view regist_key);

}  // namespace psvp::proto
