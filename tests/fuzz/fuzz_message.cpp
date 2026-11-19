// Fuzz target for what arrives after the transport's handshake: the field
// coding, the stream's messages, data chunks and their acknowledgments,
// picture and sound packets, and both ends of the network probe.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "proto/probe.h"
#include "proto/stream_message.h"
#include "proto/transport.h"
#include "proto/wire.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace psvp::proto;
    using namespace psvp::proto::transport;
    const std::span<const std::uint8_t> input(data, size);

    wire::Reader reader(input);
    while (reader.next()) {}

    if (const auto message = parse_message(input)) {
        // What was read survives being written and read again.
        const auto again = parse_message(encode_message(*message));
        if (!again || !(*again == *message)) std::abort();
    }
    if (const auto chunk = parse_data_chunk(input)) {
        if (encode_data_chunk(*chunk).size() != size) std::abort();
    }
    (void)parse_data_ack(input);
    if (const auto media = parse_media(input)) {
        if (media->header.size() + media->data.size() != size) std::abort();
    }

    // The same bytes as a datagram, and as the message inside a well-formed data chunk, for both ends.
    const TimePoint now{};
    const Endpoint console{0x0a0b0c0d, kReceiveWindow, kStreams, kStreams, 0x0a0b0c0d};
    const Endpoint client{0x01020304, kReceiveWindow, kStreams, kStreams, 0x01020304};
    ClientProbe client_probe(client.tag, console, [] { return 7U; });
    (void)client_probe.start(now);
    ConsoleProbe console_probe(console.tag, client, {});
    (void)client_probe.on_datagram(input, now);
    (void)console_probe.on_datagram(input, now);

    ControlPacket to_client;
    to_client.tag = client.tag;
    to_client.chunk_type = chunk::kData;
    to_client.chunk_flags = 1;
    to_client.payload = encode_data_chunk(DataChunk{console.first_sequence, 1, data_type::kMessage, {input.begin(), input.end()}, 0});
    ControlPacket to_console = to_client;
    to_console.tag = console.tag;
    to_console.payload = encode_data_chunk(DataChunk{client.first_sequence, 1, data_type::kMessage, {input.begin(), input.end()}, 0});
    if (to_client.payload.size() <= 0xffff - 4) {
        (void)client_probe.on_datagram(encode_control(to_client), now);
        (void)console_probe.on_datagram(encode_control(to_console), now);
    }
    (void)client_probe.on_timeout(client_probe.deadline());
    (void)console_probe.take_events();
    return 0;
}
