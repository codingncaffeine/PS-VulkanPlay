// Fuzz target for what a console sends when a session starts: the answer to
// the session request, the answer on the control channel, and then a stream of
// control messages cut into arbitrary pieces.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string_view>

#include "proto/session.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace psvp::proto;
    static const Key16 nonce{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    static const Key16 secret{16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
    const RpCipher cipher = make_session_cipher(size % 2 == 0 ? ConsoleKind::ps5 : ConsoleKind::ps4, nonce, secret);

    const std::string_view text(reinterpret_cast<const char*>(data), size);
    if (const auto parsed = parse_session_response(text); !parsed) (void)describe(parsed.error());
    if (const auto accepted = parse_ctrl_response(cipher, text)) {
        if (accepted->next_remote_number > 1) std::abort();
    } else {
        (void)describe(accepted.error());
    }

    // As a message stream: the first byte of each piece says how long the piece is.
    CtrlChannel channel(cipher, 5, 1);
    std::span<const std::uint8_t> rest(data, size);
    while (!rest.empty()) {
        const std::size_t piece = std::min<std::size_t>(rest.size() - 1, rest[0]);
        if (!channel.feed(rest.subspan(1, piece))) break;
        while (const auto message = channel.next()) {
            if (message->payload.size() + kCtrlHeaderSize > kMaxCtrlMessage) std::abort();
            (void)parse_session_id(message->payload);
        }
        rest = rest.subspan(1 + piece);
    }
    return 0;
}
