#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "check.h"
#include "proto/base64.h"
#include "proto/hex.h"
#include "proto/session.h"
#include "proto/text_message.h"
#include "vectors_regist.h"

using psvp::proto::ConsoleKind;
using psvp::proto::CtrlChannel;
using psvp::proto::Key16;
using psvp::proto::SessionRefusal;

namespace {

constexpr Key16 kConsoleNonce{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
constexpr Key16 kSecret{0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf};

std::vector<std::uint8_t> bytes_of(std::string_view text) { return {text.begin(), text.end()}; }

// What the console reads out of an encrypted header of the control request.
std::vector<std::uint8_t> opened(const psvp::proto::RpCipher& cipher, const std::string& request, std::string_view name,
                                 std::uint64_t number) {
    const auto message = psvp::proto::parse_header_block(std::string_view(request).substr(request.find('\n') + 1));
    return psvp::proto::decrypt(cipher, number, psvp::proto::base64_decode(message.value().find(name).value()).value());
}

}  // namespace

// The expected values were computed by the reference implementation (tools/oracle/run.sh --vectors).
PSVP_TEST(session_cipher_matches_the_reference) {
    using namespace psvp::test::vectors;
    const auto ps5 = psvp::proto::make_session_cipher(ConsoleKind::ps5, kConsoleNonce, kSecret);
    const auto ps4 = psvp::proto::make_session_cipher(ConsoleKind::ps4, kConsoleNonce, kSecret);
    CHECK(psvp::proto::hex_encode(ps5.key) == k_ps5_session_key);
    CHECK(psvp::proto::hex_encode(ps5.nonce) == k_ps5_session_nonce);
    CHECK(psvp::proto::hex_encode(ps4.key) == k_ps4_session_key);
    CHECK(psvp::proto::hex_encode(ps4.nonce) == k_ps4_session_nonce);
    CHECK(ps5.kind == ConsoleKind::ps5 && ps4.kind == ConsoleKind::ps4);
}

PSVP_TEST(session_request_is_exact) {
    CHECK(psvp::proto::build_session_request(ConsoleKind::ps5, "192.168.1.20", "1a2b3c4d") ==
          "GET /sie/ps5/rp/sess/init HTTP/1.1\r\n"
          "Host: 192.168.1.20:9295\r\n"
          "User-Agent: remoteplay Windows\r\n"
          "Connection: close\r\n"
          "Content-Length: 0\r\n"
          "RP-Registkey: 3161326233633464\r\n"
          "Rp-Version: 1.0\r\n"
          "\r\n");
    const std::string ps4 = psvp::proto::build_session_request(ConsoleKind::ps4, "10.0.0.2", "0a1b2c3d");
    CHECK(ps4.starts_with("GET /sie/ps4/rp/sess/init HTTP/1.1\r\nHost: 10.0.0.2:9295\r\n"));
    CHECK(ps4.find("Rp-Version: 10.0\r\n") != std::string::npos);
}

PSVP_TEST(session_response_gives_the_nonce) {
    const std::string head = "HTTP/1.1 200 OK\r\nRP-Nonce: " + psvp::proto::base64_encode(kConsoleNonce) + "\r\nRP-Version: 1.0\r\n\r\n";
    CHECK(psvp::proto::parse_session_response(head) == kConsoleNonce);
}

PSVP_TEST(session_response_names_each_refusal) {
    const auto refusal_of = [](std::string_view reason) {
        const std::string head = "HTTP/1.1 403 Forbidden\r\nRP-Application-Reason: " + std::string(reason) + "\r\nRP-Version: 9.0\r\n\r\n";
        const auto parsed = psvp::proto::parse_session_response(head);
        CHECK(!parsed.has_value());
        return parsed.error();
    };
    CHECK(refusal_of("80108b10").refusal == SessionRefusal::in_use);
    CHECK(refusal_of("80108b15").refusal == SessionRefusal::crashed);
    CHECK(refusal_of("80108b11").refusal == SessionRefusal::version_mismatch);
    CHECK(refusal_of("80108b02").refusal == SessionRefusal::invalid_account);
    CHECK(refusal_of("80108b09").refusal == SessionRefusal::not_registered);
    CHECK(refusal_of("80108bff").refusal == SessionRefusal::other);
    CHECK(refusal_of("80108b11").console_version == "9.0");
    CHECK(refusal_of("80108b11").status == 403);
    CHECK(psvp::proto::describe(refusal_of("80108b11")).find("9.0") != std::string::npos);
    CHECK(psvp::proto::describe(refusal_of("80108b10")).find("already in use") != std::string::npos);
    CHECK(psvp::proto::describe(refusal_of("80108bff")).find("80108bff") != std::string::npos);
}

PSVP_TEST(session_response_refuses_nonsense) {
    const auto refusal_of = [](std::string_view head) {
        const auto parsed = psvp::proto::parse_session_response(head);
        return parsed ? SessionRefusal::other : parsed.error().refusal;
    };
    CHECK(refusal_of("") == SessionRefusal::malformed);
    CHECK(refusal_of("HTTP/1.1 200 OK\r\n\r\n") == SessionRefusal::malformed);
    CHECK(refusal_of("HTTP/1.1 200 OK\r\nRP-Nonce: AAAA\r\n\r\n") == SessionRefusal::malformed);
    CHECK(refusal_of("HTTP/1.1 200 OK\r\nRP-Nonce: not base64 at all!!\r\n\r\n") == SessionRefusal::malformed);
}

PSVP_TEST(session_device_id_has_its_fixed_parts) {
    const auto id = psvp::proto::make_device_id(kConsoleNonce);
    CHECK(psvp::proto::hex_encode(id) == "00180000000700400080" "00112233445566778899aabbccddeeff" "000000000000");
}

PSVP_TEST(session_control_request_carries_what_the_console_expects) {
    const auto cipher = psvp::proto::make_session_cipher(ConsoleKind::ps5, kConsoleNonce, kSecret);
    const auto id = psvp::proto::make_device_id(kConsoleNonce);
    const auto request = psvp::proto::build_ctrl_request(cipher, "192.168.1.20", "1a2b3c4d", id, psvp::proto::Codec::hevc_hdr);
    CHECK(request.text.starts_with("GET /sie/ps5/rp/sess/ctrl HTTP/1.1\r\nHost: 192.168.1.20:9295\r\nUser-Agent: remoteplay Windows\r\n"
                                   "Connection: keep-alive\r\nContent-Length: 0\r\nRP-Auth: "));
    CHECK(request.text.ends_with("\r\n\r\n"));
    CHECK(request.text.find("RP-Version: 1.0\r\n") != std::string::npos);
    CHECK(request.text.find("RP-ControllerType: 3\r\nRP-ClientType: 11\r\n") != std::string::npos);
    CHECK(request.text.find("RP-ConPath: 1\r\nRP-PadProcNo: 3\r\nRP-StartBitrate: ") != std::string::npos);
    CHECK(request.next_local_number == 5);

    std::vector<std::uint8_t> key_field = bytes_of("1a2b3c4d");
    key_field.resize(16, 0);
    CHECK(opened(cipher, request.text, "RP-Auth", 0) == key_field);
    CHECK(opened(cipher, request.text, "RP-Did", 1) == std::vector<std::uint8_t>(id.begin(), id.end()));
    CHECK(opened(cipher, request.text, "RP-OSType", 2) == bytes_of(std::string("Win10.0.0\0", 10)));
    CHECK(opened(cipher, request.text, "RP-StartBitrate", 3) == std::vector<std::uint8_t>(4, 0));
    CHECK(opened(cipher, request.text, "RP-StreamingType", 4) == (std::vector<std::uint8_t>{3, 0, 0, 0}));
}

PSVP_TEST(session_control_request_for_a_ps4_has_no_ps5_headers) {
    const auto cipher = psvp::proto::make_session_cipher(ConsoleKind::ps4, kConsoleNonce, kSecret);
    const auto request = psvp::proto::build_ctrl_request(cipher, "10.0.0.2", "0a1b2c3d", psvp::proto::make_device_id(kConsoleNonce),
                                                         psvp::proto::Codec::h264);
    CHECK(request.text.starts_with("GET /sie/ps4/rp/sess/ctrl HTTP/1.1\r\n"));
    CHECK(request.text.find("RP-Version: 10.0\r\n") != std::string::npos);
    CHECK(request.text.find("RP-PadProcNo") == std::string::npos);
    CHECK(request.text.find("RP-StreamingType") == std::string::npos);
    CHECK(request.next_local_number == 4);
}

PSVP_TEST(session_control_response_tells_the_model) {
    const auto cipher = psvp::proto::make_session_cipher(ConsoleKind::ps5, kConsoleNonce, kSecret);
    const auto head_for = [&](std::uint8_t model, std::size_t size) {
        std::vector<std::uint8_t> server_type(size, 0);
        if (size > 0) server_type[0] = model;
        return "HTTP/1.1 200 OK\r\nRP-Server-Type: " + psvp::proto::base64_encode(psvp::proto::encrypt(cipher, 0, server_type)) + "\r\n\r\n";
    };
    const auto ps5 = psvp::proto::parse_ctrl_response(cipher, head_for(2, 16));
    CHECK(ps5.has_value() && ps5->model == psvp::proto::ServerModel::ps5 && ps5->next_remote_number == 1 && !ps5->prohibited);
    CHECK(psvp::proto::parse_ctrl_response(cipher, head_for(0, 16)).value().model == psvp::proto::ServerModel::ps4);
    CHECK(psvp::proto::parse_ctrl_response(cipher, head_for(1, 16)).value().model == psvp::proto::ServerModel::ps4_pro);
    CHECK(psvp::proto::parse_ctrl_response(cipher, head_for(9, 16)).value().model == psvp::proto::ServerModel::unknown);
    // Not sixteen bytes: not a server type, and no number was used for it.
    const auto odd = psvp::proto::parse_ctrl_response(cipher, head_for(2, 8));
    CHECK(odd.has_value() && odd->model == psvp::proto::ServerModel::unknown && odd->next_remote_number == 0);
    const auto bare = psvp::proto::parse_ctrl_response(cipher, "HTTP/1.1 200 OK\r\nRP-Prohibit: 1\r\n\r\n");
    CHECK(bare.has_value() && bare->prohibited && bare->next_remote_number == 0);
}

PSVP_TEST(session_control_response_reports_a_refusal) {
    const auto cipher = psvp::proto::make_session_cipher(ConsoleKind::ps5, kConsoleNonce, kSecret);
    const auto refused = psvp::proto::parse_ctrl_response(cipher, "HTTP/1.1 403 Forbidden\r\n\r\n");
    CHECK(!refused.has_value() && !refused.error().malformed && refused.error().status == 403);
    CHECK(psvp::proto::describe(refused.error()).find("403") != std::string::npos);
    const auto garbage = psvp::proto::parse_ctrl_response(cipher, "garbage");
    CHECK(!garbage.has_value() && garbage.error().malformed);
    const auto bad_base64 = psvp::proto::parse_ctrl_response(cipher, "HTTP/1.1 200 OK\r\nRP-Server-Type: ***\r\n\r\n");
    CHECK(!bad_base64.has_value() && bad_base64.error().malformed);
}

PSVP_TEST(control_messages_cross_the_channel_both_ways) {
    const auto cipher = psvp::proto::make_session_cipher(ConsoleKind::ps5, kConsoleNonce, kSecret);
    CtrlChannel client(cipher, 5, 1);
    CtrlChannel console(cipher, 1, 5);
    const auto id = bytes_of("JABCDEFGHIJKLMNOPQRSTUVWX0123");
    const auto sent = console.encode(psvp::proto::ctrl_type::kSessionId, id).value();
    CHECK(sent.size() == 8 + id.size());
    CHECK(sent[3] == id.size() && sent[4] == 0x00 && sent[5] == 0x33 && sent[6] == 0 && sent[7] == 0);
    CHECK(std::vector<std::uint8_t>(sent.begin() + 8, sent.end()) != id);  // the payload travels encrypted
    CHECK(client.feed(sent));
    const auto received = client.next();
    CHECK(received.has_value() && received->type == 0x33 && received->payload == id);
    CHECK(!client.next().has_value());

    const auto reply = client.encode(psvp::proto::ctrl_type::kLoginPinReply, bytes_of("4321")).value();
    CHECK(reply[4] == 0x80 && reply[5] == 0x04);
    CHECK(console.feed(reply));
    CHECK(console.next().value().payload == bytes_of("4321"));
}

// A message without a payload takes no number: the next one with a payload must
// still open on the other side.
PSVP_TEST(control_messages_without_a_payload_take_no_number) {
    const auto cipher = psvp::proto::make_session_cipher(ConsoleKind::ps4, kConsoleNonce, kSecret);
    CtrlChannel client(cipher, 4, 1);
    CtrlChannel console(cipher, 1, 4);
    for (int round = 0; round < 3; ++round) {
        const auto heartbeat = client.encode(psvp::proto::ctrl_type::kHeartbeatReply).value();
        CHECK(heartbeat.size() == 8 && heartbeat[4] == 0x01 && heartbeat[5] == 0xfe);
        CHECK(console.feed(heartbeat));
        const auto empty = console.next();
        CHECK(empty.has_value() && empty->type == 0x1fe && empty->payload.empty());
        const auto text = bytes_of("round " + std::to_string(round));
        CHECK(console.feed(client.encode(0x8004, text).value()));
        CHECK(console.next().value().payload == text);
    }
}

PSVP_TEST(control_messages_arrive_in_any_pieces) {
    const auto cipher = psvp::proto::make_session_cipher(ConsoleKind::ps5, kConsoleNonce, kSecret);
    CtrlChannel sender(cipher, 1, 5);
    CtrlChannel receiver(cipher, 5, 1);
    std::vector<std::uint8_t> stream;
    for (int i = 0; i < 5; ++i) {
        const auto bytes = sender.encode(static_cast<std::uint16_t>(0x100 + i), bytes_of(std::string(static_cast<std::size_t>(i) * 7, 'x'))).value();
        stream.insert(stream.end(), bytes.begin(), bytes.end());
    }
    int received = 0;
    for (const std::uint8_t byte : stream) {  // one byte at a time
        CHECK(receiver.feed(std::span(&byte, 1)));
        while (const auto message = receiver.next()) {
            CHECK(message->type == 0x100 + received);
            CHECK(message->payload == bytes_of(std::string(static_cast<std::size_t>(received) * 7, 'x')));
            ++received;
        }
    }
    CHECK(received == 5);
}

PSVP_TEST(control_messages_have_a_size_limit) {
    const auto cipher = psvp::proto::make_session_cipher(ConsoleKind::ps5, kConsoleNonce, kSecret);
    CtrlChannel channel(cipher, 5, 1);
    CHECK(channel.encode(1, std::vector<std::uint8_t>(504, 0)).has_value());
    CHECK(!channel.encode(1, std::vector<std::uint8_t>(505, 0)).has_value());

    // A header that announces more than the limit breaks the stream for good.
    const std::vector<std::uint8_t> too_long{0x00, 0x00, 0x01, 0xf9, 0x00, 0x33, 0x00, 0x00};
    CHECK(channel.feed(too_long));
    CHECK(!channel.next().has_value());
    CHECK(!channel.feed(too_long));
    const std::vector<std::uint8_t> huge{0xff, 0xff, 0xff, 0xff, 0x00, 0x33, 0x00, 0x00};
    CtrlChannel other(cipher, 5, 1);
    CHECK(other.feed(huge) && !other.next().has_value() && !other.feed(huge));
}

PSVP_TEST(session_id_is_read_strictly) {
    using psvp::proto::parse_session_id;
    CHECK(parse_session_id(bytes_of("JABCDEFGHIJKLMNOPQRSTUVWX")) == "ABCDEFGHIJKLMNOPQRSTUVWX");
    CHECK(parse_session_id(bytes_of("J" + std::string(78, 'a'))).has_value());
    CHECK(!parse_session_id(bytes_of("J" + std::string(79, 'a'))).has_value());
    CHECK(!parse_session_id(bytes_of("J" + std::string(23, 'a'))).has_value());
    CHECK(!parse_session_id(bytes_of("K" + std::string(24, 'a'))).has_value());
    CHECK(!parse_session_id(bytes_of("J" + std::string(23, 'a') + "-")).has_value());
    CHECK(!parse_session_id({}).has_value());
}
