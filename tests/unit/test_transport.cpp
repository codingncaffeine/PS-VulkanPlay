#include <array>
#include <cstdint>
#include <vector>

#include "check.h"
#include "proto/hex.h"
#include "proto/transport.h"

using namespace psvp::proto::transport;
using namespace std::chrono_literals;

namespace {

std::string hex(const std::vector<std::uint8_t>& bytes) { return psvp::proto::hex_encode(bytes); }

std::array<std::uint8_t, kCookieSize> cookie_of(std::uint8_t first) {
    std::array<std::uint8_t, kCookieSize> cookie{};
    for (std::size_t i = 0; i < cookie.size(); ++i) cookie[i] = static_cast<std::uint8_t>(first + i);
    return cookie;
}

const TimePoint kStart{};

}  // namespace

// Byte for byte as docs/protocol/transport.md lays it out.
PSVP_TEST(transport_init_packet_is_exact) {
    ClientHandshake client(0x11223344);
    const auto init = client.start(kStart);
    CHECK(init.has_value());
    if (!init) return;
    CHECK(hex(*init) == "00"                  // a control packet
                        "00000000"            // the console's tag, not known yet
                        "00000000"            // authentication code
                        "00000000"            // key position
                        "01" "00" "0014"      // init, no flags, sixteen bytes of payload plus four
                        "11223344"            // the client's tag
                        "00019000"            // receive window
                        "0064" "0064"         // streams out and in
                        "11223344");          // first sequence number: the tag
    CHECK(client.state() == ClientHandshake::State::awaiting_init_ack);
    CHECK(client.deadline() == kStart + 5s);
}

PSVP_TEST(transport_control_packets_read_back) {
    ControlPacket packet;
    packet.tag = 0xa1b2c3d4;
    packet.mac = {1, 2, 3, 4};
    packet.key_position = 0x01020304;
    packet.chunk_type = chunk::kData;
    packet.chunk_flags = 7;
    packet.payload = {9, 8, 7};
    const auto bytes = encode_control(packet);
    CHECK(bytes.size() == 1 + 16 + 3);
    const auto back = parse_control(bytes);
    CHECK(back.has_value());
    if (!back) return;
    CHECK(back->tag == packet.tag && back->mac == packet.mac && back->key_position == packet.key_position);
    CHECK(back->chunk_type == chunk::kData && back->chunk_flags == 7 && back->payload == packet.payload);
}

PSVP_TEST(transport_refuses_packets_that_do_not_add_up) {
    ControlPacket packet;
    packet.payload = {1, 2, 3, 4};
    auto bytes = encode_control(packet);
    CHECK(parse_control(bytes).has_value());
    auto shorter = bytes;
    shorter.pop_back();
    CHECK(!parse_control(shorter).has_value());
    auto longer = bytes;
    longer.push_back(0);
    CHECK(!parse_control(longer).has_value());
    auto video = bytes;
    video[0] = packet_kind::kVideo;
    CHECK(!parse_control(video).has_value());
    CHECK(!parse_control({}).has_value());
    CHECK(!parse_control(std::vector<std::uint8_t>(16, 0)).has_value());
    auto tiny_size = bytes;
    tiny_size[15] = 0;
    tiny_size[16] = 3;  // a size field below four cannot be
    CHECK(!parse_control(tiny_size).has_value());
}

PSVP_TEST(transport_handshake_completes_between_both_sides) {
    ClientHandshake client(0x11223344);
    ConsoleHandshake console(0x55667788, cookie_of(0x40));

    const auto init = client.start(kStart).value();
    const auto init_ack = console.on_datagram(init);
    CHECK(init_ack.has_value() && !console.connected());
    CHECK(console.client().has_value() && console.client()->tag == 0x11223344 && console.client()->first_sequence == 0x11223344);
    CHECK(console.client()->receive_window == 0x19000 && console.client()->outbound_streams == 100);

    const auto cookie = client.on_datagram(*init_ack, kStart + 10ms);
    CHECK(cookie.has_value());
    CHECK(client.state() == ClientHandshake::State::awaiting_cookie_ack);
    CHECK(client.console().tag == 0x55667788 && client.console().first_sequence == 0x55667788);
    // The cookie goes back unchanged, addressed with the console's tag.
    const auto cookie_packet = parse_control(*cookie).value();
    CHECK(cookie_packet.tag == 0x55667788 && cookie_packet.chunk_type == chunk::kCookie);
    const auto expected_cookie = cookie_of(0x40);
    CHECK(cookie_packet.payload == std::vector<std::uint8_t>(expected_cookie.begin(), expected_cookie.end()));

    const auto cookie_ack = console.on_datagram(*cookie);
    CHECK(cookie_ack.has_value() && console.connected());
    CHECK(!client.on_datagram(*cookie_ack, kStart + 20ms).has_value());
    CHECK(client.state() == ClientHandshake::State::connected);
}

PSVP_TEST(transport_handshake_sends_again_and_gives_up_after_three_tries) {
    ClientHandshake client(0x11223344);
    const auto first = client.start(kStart).value();
    CHECK(!client.on_timeout(kStart + 4999ms).has_value());  // not yet
    const auto second = client.on_timeout(kStart + 5s);
    CHECK(second.has_value() && *second == first);
    CHECK(client.deadline() == kStart + 10s);
    const auto third = client.on_timeout(kStart + 10s);
    CHECK(third.has_value() && *third == first);
    CHECK(!client.on_timeout(kStart + 15s).has_value());
    CHECK(client.state() == ClientHandshake::State::failed);
    CHECK(client.failure().find("did not answer") != std::string::npos);
}

PSVP_TEST(transport_handshake_resends_the_cookie_and_ignores_a_repeated_init_ack) {
    ClientHandshake client(0x11223344);
    ConsoleHandshake console(0x55667788, cookie_of(1));
    const auto init_ack = console.on_datagram(client.start(kStart).value()).value();
    const auto cookie = client.on_datagram(init_ack, kStart).value();
    CHECK(!client.on_datagram(init_ack, kStart + 1s).has_value());  // the same acknowledgment again
    CHECK(client.state() == ClientHandshake::State::awaiting_cookie_ack);
    const auto again = client.on_timeout(kStart + 5s);
    CHECK(again.has_value() && *again == cookie);
    CHECK(client.on_timeout(kStart + 10s).has_value());
    CHECK(!client.on_timeout(kStart + 15s).has_value());
    CHECK(client.state() == ClientHandshake::State::failed);
    CHECK(client.failure().find("cookie") != std::string::npos);
}

PSVP_TEST(transport_handshake_ignores_what_is_not_for_it) {
    ClientHandshake client(0x11223344);
    (void)client.start(kStart);
    ConsoleHandshake other_console(0x55667788, cookie_of(1));
    ClientHandshake other_client(0x99999999);
    const auto foreign_ack = other_console.on_datagram(other_client.start(kStart).value()).value();
    CHECK(!client.on_datagram(foreign_ack, kStart).has_value());  // another connection's tag
    CHECK(!client.on_datagram(std::vector<std::uint8_t>{2, 0, 0}, kStart).has_value());  // a video packet
    CHECK(client.state() == ClientHandshake::State::awaiting_init_ack);
}

PSVP_TEST(transport_handshake_refuses_a_console_it_cannot_work_with) {
    const auto ack_with = [](std::uint32_t tag, std::uint16_t outbound, std::uint16_t inbound) {
        ControlPacket packet;
        packet.tag = 0x11223344;
        packet.chunk_type = chunk::kInitAck;
        const std::uint8_t fields[16] = {static_cast<std::uint8_t>(tag >> 24), static_cast<std::uint8_t>(tag >> 16),
                                         static_cast<std::uint8_t>(tag >> 8),  static_cast<std::uint8_t>(tag),
                                         0, 1, 0x90, 0,
                                         static_cast<std::uint8_t>(outbound >> 8), static_cast<std::uint8_t>(outbound),
                                         static_cast<std::uint8_t>(inbound >> 8),  static_cast<std::uint8_t>(inbound),
                                         0, 0, 0, 1};
        packet.payload.assign(fields, fields + 16);
        packet.payload.resize(16 + kCookieSize, 0x5a);
        return encode_control(packet);
    };
    const auto state_after = [&](std::uint32_t tag, std::uint16_t outbound, std::uint16_t inbound) {
        ClientHandshake client(0x11223344);
        (void)client.start(kStart);
        (void)client.on_datagram(ack_with(tag, outbound, inbound), kStart);
        return client.state();
    };
    CHECK(state_after(0x55667788, 100, 100) == ClientHandshake::State::awaiting_cookie_ack);
    CHECK(state_after(0x55667788, 50, 200) == ClientHandshake::State::awaiting_cookie_ack);
    CHECK(state_after(0, 100, 100) == ClientHandshake::State::failed);
    CHECK(state_after(0x55667788, 0, 100) == ClientHandshake::State::failed);
    CHECK(state_after(0x55667788, 100, 0) == ClientHandshake::State::failed);
    CHECK(state_after(0x55667788, 101, 100) == ClientHandshake::State::failed);
    CHECK(state_after(0x55667788, 100, 99) == ClientHandshake::State::failed);
}

PSVP_TEST(transport_console_side_refuses_a_wrong_cookie) {
    ClientHandshake client(0x11223344);
    ConsoleHandshake console(0x55667788, cookie_of(1));
    (void)console.on_datagram(client.start(kStart).value());
    ControlPacket wrong;
    wrong.tag = 0x55667788;
    wrong.chunk_type = chunk::kCookie;
    wrong.payload.assign(kCookieSize, 0);
    CHECK(!console.on_datagram(encode_control(wrong)).has_value());
    CHECK(!console.connected());
    wrong.tag = 0x12345678;  // not the console's tag
    const auto right_cookie = cookie_of(1);
    wrong.payload.assign(right_cookie.begin(), right_cookie.end());
    CHECK(!console.on_datagram(encode_control(wrong)).has_value());
    CHECK(!console.connected());
}

// Below the ceiling a report is what was counted; above it, the ceiling's share of the same packets,
// rounded down, and the rest count as arrived.
PSVP_TEST(transport_loss_ceiling_lowers_only_what_is_above_it) {
    CHECK(with_loss_ceiling({95, 5}, 5) == (PacketCounts{95, 5}));
    CHECK(with_loss_ceiling({94, 6}, 5) == (PacketCounts{95, 5}));
    CHECK(with_loss_ceiling({0, 200}, 5) == (PacketCounts{190, 10}));
    CHECK(with_loss_ceiling({10, 9}, 5) == (PacketCounts{19, 0}));
    CHECK(with_loss_ceiling({0, 0}, 5) == (PacketCounts{0, 0}));
    CHECK(with_loss_ceiling({30, 70}, 100) == (PacketCounts{30, 70}));
    CHECK(with_loss_ceiling({30, 70}, 250) == (PacketCounts{30, 70}));
    CHECK(with_loss_ceiling({30, 70}, 0) == (PacketCounts{100, 0}));
}
