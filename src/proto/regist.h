// Registering this client with a console: the request it sends and the answer
// it gets back. docs/protocol/registration.md describes the exchange.
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "proto/console.h"
#include "proto/rp_cipher.h"

namespace psvp::proto {

inline constexpr std::size_t kAccountIdSize = 8;
using AccountId = std::array<std::uint8_t, kAccountIdSize>;

// Before the request, a datagram asks whether the console is taking
// registrations (it does while it shows its PIN). Sent to kSessionPort.
[[nodiscard]] std::string regist_search_packet(ConsoleKind kind);
[[nodiscard]] bool is_regist_search_reply(ConsoleKind kind, std::span<const std::uint8_t> datagram);

struct RegistRequest {
    std::string head;                   // request line and headers, ending with the blank line
    std::vector<std::uint8_t> payload;  // follows the head on the same connection
    RpCipher cipher;                    // opens the response
};

// `pin` is the eight-digit number the console shows; `nonce` must be fresh random bytes.
[[nodiscard]] RegistRequest build_regist_request(ConsoleKind kind, const AccountId& account_id, std::uint32_t pin,
                                                 const Key16& nonce);

// What a console hands out when it accepts a registration.
struct RegisteredConsole {
    ConsoleKind kind = ConsoleKind::ps5;
    std::string nickname;
    std::array<std::uint8_t, 6> mac{};
    std::string regist_key;        // up to sixteen characters; identifies this client to the console
    Key16 key{};                   // the secret shared with the console
    std::uint32_t key_type = 0;
    std::string ap_ssid;           // the console's own access point, for a direct connection
    std::string ap_bssid;
    std::string ap_key;
    std::string ap_name;
};

enum class RegistFailure {
    malformed,   // the response is not what a console sends
    refused,     // the console answered with an error status
    incomplete,  // the response lacks the registration key, the secret or the address
};

struct RegistError {
    RegistFailure failure = RegistFailure::malformed;
    int status = 0;            // the response's status code, when it had one
    std::uint32_t reason = 0;  // the console's reason code, when it gave one
};

[[nodiscard]] std::string describe(const RegistError& error);

// The response head is everything up to and including its blank line. Returns
// the number of body bytes that follow it.
[[nodiscard]] std::expected<std::size_t, RegistError> parse_regist_response_head(std::string_view head);

[[nodiscard]] std::expected<RegisteredConsole, RegistError> parse_regist_response_body(
    const RpCipher& cipher, std::span<const std::uint8_t> body);

// Where things sit in the request payload. The console side (the mock console) reads them back.
namespace regist_layout {
inline constexpr std::size_t kInnerOffset = 0x1e0;      // the encrypted text starts here
inline constexpr std::size_t kKeyOffsetByte = 0x18d;    // its low five bits pick the key
inline constexpr std::size_t kWrapOffsetByte = 0x000;   // its high five bits pick the nonce wrapping
inline constexpr std::size_t kWrappedLowAt = 0x191;     // wrapped nonce, bytes 0 to 7
inline constexpr std::size_t kWrappedHighAt = 0x0c7;    // wrapped nonce, bytes 8 to 15
inline constexpr std::uint8_t kFiller = 'A';
}  // namespace regist_layout

// The largest body a registration response may announce.
inline constexpr std::size_t kMaxRegistBody = 4096;

}  // namespace psvp::proto
