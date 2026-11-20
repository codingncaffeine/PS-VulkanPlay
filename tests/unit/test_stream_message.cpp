#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "check.h"
#include "proto/hex.h"
#include "proto/stream_message.h"
#include "proto/wire.h"

using namespace psvp::proto;

namespace {

std::string hex(const std::vector<std::uint8_t>& bytes) { return hex_encode(bytes); }

std::vector<std::uint8_t> unhex(std::string_view text) { return hex_decode(text).value(); }

}  // namespace

PSVP_TEST(wire_writes_numbers_in_seven_bit_groups) {
    wire::Writer w;
    w.varint(1, 0);
    w.varint(2, 127);
    w.varint(3, 128);
    w.varint(4, 1454);
    w.varint(31, 0xffffffffffffffffULL);
    CHECK(hex(w.data()) == "0800" "107f" "188001" "20ae0b" "f801ffffffffffffffffff01");
}

PSVP_TEST(wire_reads_back_what_it_wrote) {
    wire::Writer inner;
    inner.varint(1, 300);
    wire::Writer w;
    w.varint(1, 12);
    w.text(2, "hello");
    w.message(14, inner);
    wire::Reader r(w.data());
    auto f = r.next();
    CHECK(f && f->number == 1 && f->kind == wire::Kind::varint && f->value == 12);
    f = r.next();
    CHECK(f && f->number == 2 && f->kind == wire::Kind::bytes && f->bytes.size() == 5 && f->bytes[0] == 'h');
    f = r.next();
    CHECK(f && f->number == 14 && f->kind == wire::Kind::bytes && f->bytes.size() == 3);
    CHECK(!r.next());
    CHECK(r.ok());
}

PSVP_TEST(wire_reads_fixed_width_fields) {
    const auto bytes = unhex("0d78563412" "110807060504030201");
    wire::Reader r(bytes);
    auto f = r.next();
    CHECK(f && f->number == 1 && f->kind == wire::Kind::fixed32 && f->value == 0x12345678);
    f = r.next();
    CHECK(f && f->number == 2 && f->kind == wire::Kind::fixed64 && f->value == 0x0102030405060708ULL);
    CHECK(!r.next());
    CHECK(r.ok());
}

PSVP_TEST(wire_refuses_what_is_cut_short_or_malformed) {
    for (const std::string_view bad : {"08",                        // a key without its value
                                       "0880",                      // a number that never ends
                                       "08ffffffffffffffffff02",    // more than 64 bits
                                       "1205aabb",                  // a length past the end
                                       "0d010203",                  // four bytes promised, three there
                                       "0b",                        // a group
                                       "00"}) {                     // field number zero
        const auto bytes = unhex(bad);
        wire::Reader r(bytes);
        while (r.next()) {}
        CHECK(!r.ok());
    }
}

// The exact bytes of the probe's messages; the sizes are those of the 33-byte
// datagram other clients were seen to send first (7 bytes of message) and of
// the buffers the reference encodes into.
PSVP_TEST(message_bytes_of_the_probe_are_exact) {
    CHECK(hex(encode_message(make_message(ProtocolRequest{{9}}))) == "081f" "fa0102" "0809");
    BigMessage big;
    big.client_version = 9;
    CHECK(hex(encode_message(make_message(big))) == "0800" "1208" "0809" "1200" "1a00" "2200");
    CHECK(hex(encode_message(make_message(ProbeEcho{true}))) == "080c" "7206" "0800" "1202" "0801");
    CHECK(hex(encode_message(make_message(ProbeEcho{false}))) == "080c" "7206" "0800" "1202" "0800");
    ProbeMtu mtu;
    mtu.id = 1;
    mtu.mtu_request = 1454;
    CHECK(hex(encode_message(make_message(mtu))) == "080c" "7209" "0801" "1a05" "0801" "10ae0b");
    CHECK(hex(encode_message(make_message(ProbeClientMtu{1, 1454, true, 1454}))) ==
          "080c" "720e" "0804" "2a0a" "0801" "10ae0b" "1801" "20ae0b");
    const auto goodbye = encode_message(make_message(DisconnectMessage{"Client Disconnecting"}));
    CHECK(goodbye.size() == 26);
    CHECK(hex(goodbye) == "0808" "5216" "0a14" + hex_encode(std::vector<std::uint8_t>{'C', 'l', 'i', 'e', 'n', 't', ' ', 'D', 'i', 's', 'c', 'o',
                                                                                      'n', 'n', 'e', 'c', 't', 'i', 'n', 'g'}));
}

PSVP_TEST(message_survives_writing_and_reading) {
    BigMessage big;
    big.client_version = 12;
    big.session_key = "key";
    big.launch_spec = "spec";
    big.encrypted_key = {0, 1, 2, 3};
    big.ecdh_public_key = std::vector<std::uint8_t>{9, 8, 7};
    BangMessage bang;
    bang.server_version = 12;
    bang.token = 0xfffffffe;
    bang.encrypted_key_accepted = true;
    bang.version_accepted = true;
    bang.session_key = "session";
    bang.ecdh_signature = std::vector<std::uint8_t>{};
    ProbeMtu mtu{7, 576, 1, 2, 3, 4};
    for (const StreamMessage& message :
         {make_message(big), make_message(bang), make_message(DisconnectMessage{"bye"}), make_message(ProbeEcho{true}), make_message(mtu),
          make_message(ProbeClientMtu{2, 1000, false, std::nullopt}), make_message(ProtocolRequest{{7, 9, 12}}),
          make_message(ProtocolRequestAck{9}), make_message(ProtocolRequestAck{})}) {
        const auto read = parse_message(encode_message(message));
        CHECK(read.has_value());
        if (read) CHECK(*read == message);
    }
}

PSVP_TEST(message_of_an_unknown_type_is_read_as_its_type) {
    // Type 3 (a heartbeat) has no body here yet; an unknown field is skipped.
    const auto read = parse_message(unhex("0803" "9a0301ff"));
    CHECK(read.has_value());
    if (!read) return;
    CHECK(read->type == 3);
    CHECK(std::holds_alternative<std::monostate>(read->body));
}

PSVP_TEST(message_type_may_come_after_its_body) {
    const auto read = parse_message(unhex("7206" "0800" "1202" "0801" "080c"));
    CHECK(read.has_value());
    if (read) CHECK(read->as<ProbeEcho>() != nullptr && read->as<ProbeEcho>()->state);
}

PSVP_TEST(message_reads_packed_version_lists) {
    const auto read = parse_message(unhex("081f" "fa0104" "0a02" "0709"));
    CHECK(read.has_value());
    const std::vector<std::uint32_t> expected{7, 9};
    if (read) CHECK(read->as<ProtocolRequest>() != nullptr && read->as<ProtocolRequest>()->versions == expected);
}

PSVP_TEST(message_is_refused_when_it_is_not_one) {
    CHECK(!parse_message({}));                                 // no type
    CHECK(!parse_message(unhex("1200")));                      // a body without a type
    CHECK(!parse_message(unhex("0a0101")));                    // the type as bytes
    CHECK(!parse_message(unhex("0801" "1a02" "0809")));        // a bang without its required fields
    CHECK(!parse_message(unhex("080c" "7204" "0801" "1a00"))); // a size request without its numbers
    CHECK(!parse_message(unhex("0800" "1208" "0809" "1200" "1a00" "22")));  // cut short
}

PSVP_TEST(message_with_a_probe_command_but_no_body_has_none) {
    const auto read = parse_message(unhex("080c" "7202" "0803"));
    CHECK(read.has_value());
    if (read) CHECK(read->type == message_type::kProbe && std::holds_alternative<std::monostate>(read->body));
}

// The stream information's list of sound channels: what a console says it sends as sound, each
// with a type, a format in the form of the sound header, and whether its frames are samples as
// they are.
PSVP_TEST(stream_information_lists_sound_channels) {
    StreamInfo info;
    info.audio_header = {0x02, 0x10, 0x00, 0x00, 0xbb, 0x80, 0x00, 0x00, 0x01, 0xe0, 0x00, 0x00, 0x00, 0x01};
    info.sound_channels.push_back(SoundChannel{1, info.audio_header, false});
    info.sound_channels.push_back(SoundChannel{2, {0x02, 0x10, 0x00, 0x00, 0x0b, 0xb8, 0x00, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0x01}, true});
    info.sound_channels.push_back(SoundChannel{7, {0x01}, std::nullopt});
    const auto bytes = encode_message(make_message(info));
    const auto parsed = parse_message(bytes);
    CHECK(parsed.has_value());
    const StreamInfo* read = parsed ? parsed->as<StreamInfo>() : nullptr;
    CHECK(read != nullptr);
    if (read == nullptr) return;
    CHECK(read->sound_channels.size() == 3);
    CHECK(*read == info);
    // Without the list the message is what it was.
    StreamInfo plain;
    plain.audio_header = info.audio_header;
    const auto plain_parsed = parse_message(encode_message(make_message(plain)));
    CHECK(plain_parsed && plain_parsed->as<StreamInfo>() != nullptr && plain_parsed->as<StreamInfo>()->sound_channels.empty());
}
