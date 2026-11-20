// Fuzz target for the stream's set-up: any bytes as a datagram for either end,
// before and after the keys, as a message inside a well-formed data chunk
// (unsealed, and sealed with the right keys), as a public key and as a sealed
// launch specification.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string_view>
#include <vector>

#include "proto/key_exchange.h"
#include "proto/launch_spec.h"
#include "proto/stream_setup.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace {

using namespace psvp::proto;
using namespace psvp::proto::transport;

constexpr std::uint32_t kClientTag = 0x01020304;
constexpr std::uint32_t kConsoleTag = 0x0a0b0c0d;

struct Fixture {
    KeyExchange client_keys;
    KeyExchange console_keys;
    RpCipher session_cipher;
    Key16 handshake_key{};
    SharedSecret secret{};
};

const Fixture& fixture() {
    static const Fixture made = [] {
        std::array<std::uint8_t, 32> a{};
        std::array<std::uint8_t, 32> b{};
        for (std::size_t i = 0; i < 32; ++i) {
            a[i] = static_cast<std::uint8_t>(1 + i);
            b[i] = static_cast<std::uint8_t>(0xa1 + i);
        }
        Key16 nonce{};
        Key16 registration{};
        Key16 handshake{};
        for (std::size_t i = 0; i < 16; ++i) {
            nonce[i] = static_cast<std::uint8_t>(i * 17);
            registration[i] = static_cast<std::uint8_t>(0xa0 + i);
            handshake[i] = static_cast<std::uint8_t>(0x10 + i);
        }
        Fixture f{KeyExchange::from_private_key(a).value(), KeyExchange::from_private_key(b).value(),
                  make_session_cipher(ConsoleKind::ps5, nonce, registration), handshake, {}};
        f.secret = f.client_keys.shared_secret(f.console_keys.public_key()).value();
        return f;
    }();
    return made;
}

std::vector<std::uint8_t> chunk_for(std::uint32_t tag, std::uint32_t sequence, std::span<const std::uint8_t> message) {
    ControlPacket packet;
    packet.tag = tag;
    packet.chunk_type = chunk::kData;
    packet.chunk_flags = chunk_flag::kLast;
    packet.payload = encode_data_chunk(DataChunk{sequence, 1, data_type::kMessage, {message.begin(), message.end()}});
    return encode_control(packet);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> input(data, size);
    const Fixture& f = fixture();
    const TimePoint now{};
    const Endpoint console_end{kConsoleTag, kReceiveWindow, kStreams, kStreams, kConsoleTag};
    const Endpoint client_end{kClientTag, kReceiveWindow, kStreams, kStreams, kClientTag};

    // What arrives may be anything.
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    if (const auto opened = open_launch_spec(f.session_cipher, text)) (void)launch_spec_handshake_key(*opened);
    (void)launch_spec_handshake_key(text);
    if (size == kPublicKeySize) (void)f.client_keys.shared_secret(input);
    const StreamCipher console_cipher(kConsoleDirection, f.handshake_key, f.secret);
    const StreamCipher client_cipher(kClientDirection, f.handshake_key, f.secret);
    (void)datagram_mac(console_cipher, 0, input);
    ReceiveKeys keys(console_cipher);
    (void)verify_datagram(keys, input);

    StreamRequest request;
    request.session_id = "MockSessionIdent";
    request.session_cipher = f.session_cipher;
    ConsoleStream::Options options;
    options.session_cipher = f.session_cipher;
    options.session_id = "MockSessionIdent";
    options.resolutions.push_back(StreamResolution{1280, 720, {1, 2, 3}});
    const bool as_message = size <= 1200;

    // Before the keys: the bytes as a datagram, and as the console's first message.
    {
        ClientStream client(kClientTag, console_end, request, f.client_keys, f.handshake_key);
        (void)client.start(now);
        (void)client.on_datagram(input, now);
        if (as_message) (void)client.on_datagram(chunk_for(kClientTag, kConsoleTag, input), now);
        (void)client.on_timeout(client.deadline());
        ConsoleStream console(kConsoleTag, client_end, options, f.console_keys);
        (void)console.on_datagram(input, now);
        if (as_message) (void)console.on_datagram(chunk_for(kConsoleTag, kClientTag, input), now);
        (void)console.take_events();
    }

    // After a whole set-up: the bytes as a datagram, and as a message sealed with the right keys.
    ClientStream client(kClientTag, console_end, request, f.client_keys, f.handshake_key);
    ConsoleStream console(kConsoleTag, client_end, options, f.console_keys);
    Datagrams to_console = client.start(now);
    for (int round = 0; round < 6 && !to_console.empty(); ++round) {
        Datagrams to_client;
        for (const auto& datagram : to_console) {
            for (auto& answer : console.on_datagram(datagram, now)) to_client.push_back(std::move(answer));
        }
        to_console.clear();
        for (const auto& datagram : to_client) {
            for (auto& answer : client.on_datagram(datagram, now)) to_console.push_back(std::move(answer));
        }
    }
    if (client.state() != ClientStream::State::ready) std::abort();
    (void)client.on_datagram(input, now);
    (void)console.on_datagram(input, now);
    if (as_message) {
        // The console has sent four chunks (its answer, the stream information, and its two answers to the
        // client's stream information), the client four: the sequence numbers that come next are known.
        std::vector<std::uint8_t> sealed = chunk_for(kClientTag, kConsoleTag + 4, input);
        SendKeys console_keys(console_cipher);
        console_keys.next_position = 1 << 20;
        (void)seal_datagram(console_cipher, console_keys.take(size), sealed);
        (void)client.on_datagram(sealed, now);
        sealed = chunk_for(kConsoleTag, kClientTag + 4, input);
        SendKeys client_keys(client_cipher);
        client_keys.next_position = 1 << 20;
        (void)seal_datagram(client_cipher, client_keys.take(size), sealed);
        (void)console.on_datagram(sealed, now);
    }
    (void)client.on_timeout(client.deadline());
    (void)client.disconnect(now);
    (void)console.take_events();
    (void)console.summary();
    return 0;
}
