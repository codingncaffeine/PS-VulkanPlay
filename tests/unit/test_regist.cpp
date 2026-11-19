#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "check.h"
#include "proto/hex.h"
#include "proto/regist.h"
#include "vectors_regist.h"

using psvp::proto::ConsoleKind;
using psvp::proto::Key16;
using psvp::proto::RegistFailure;

namespace {

constexpr Key16 kNonce{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
constexpr psvp::proto::AccountId kAccount{0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
constexpr std::uint32_t kPin = 12345678;

std::vector<std::uint8_t> sealed_body(const psvp::proto::RpCipher& cipher, std::string_view text) {
    return psvp::proto::encrypt(cipher, 0, std::vector<std::uint8_t>(text.begin(), text.end()));
}

const std::string kPs5Body = "AP-Ssid: PS5-123\r\n"
                             "AP-Bssid: aabbccddeeff\r\n"
                             "AP-Key: secretkey\r\n"
                             "AP-Name: PS5\r\n"
                             "PS5-Mac: 0123456789ab\r\n"
                             "PS5-RegistKey: 6162636431323334\r\n"
                             "PS5-Nickname: Living Room\r\n"
                             "RP-KeyType: 2\r\n"
                             "RP-Key: 000102030405060708090a0b0c0d0e0f\r\n";

}  // namespace

PSVP_TEST(regist_search_packets_and_replies) {
    CHECK(psvp::proto::regist_search_packet(ConsoleKind::ps5) == std::string("SRC3\0", 5));
    CHECK(psvp::proto::regist_search_packet(ConsoleKind::ps4) == std::string("SRC2\0", 5));
    const std::vector<std::uint8_t> res3{'R', 'E', 'S', '3', 0};
    const std::vector<std::uint8_t> res2{'R', 'E', 'S', '2'};
    CHECK(psvp::proto::is_regist_search_reply(ConsoleKind::ps5, res3));
    CHECK(!psvp::proto::is_regist_search_reply(ConsoleKind::ps4, res3));
    CHECK(psvp::proto::is_regist_search_reply(ConsoleKind::ps4, res2));
    CHECK(!psvp::proto::is_regist_search_reply(ConsoleKind::ps5, std::vector<std::uint8_t>{'R', 'E', 'S'}));
    CHECK(!psvp::proto::is_regist_search_reply(ConsoleKind::ps5, {}));
}

// 587 = 480 bytes before the encrypted text + 107 bytes of it (client type and account lines).
PSVP_TEST(regist_request_head_is_exact) {
    const auto request = psvp::proto::build_regist_request(ConsoleKind::ps5, kAccount, kPin, kNonce);
    CHECK(request.payload.size() == 587);
    CHECK(request.head == "POST /sie/ps5/rp/sess/rgst HTTP/1.1\r\n HTTP/1.1\r\n"
                          "HOST: 10.0.2.15\r\n"
                          "User-Agent: remoteplay Windows\r\n"
                          "Connection: close\r\n"
                          "Content-Length: 587\r\n"
                          "RP-Version: 1.0\r\n"
                          "\r\n");
    const auto ps4 = psvp::proto::build_regist_request(ConsoleKind::ps4, kAccount, kPin, kNonce);
    CHECK(ps4.head.starts_with("POST /sie/ps4/rp/sess/rgst HTTP/1.1\r\n"));
    CHECK(ps4.head.find("RP-Version: 10.0\r\n") != std::string::npos);
}

PSVP_TEST(regist_request_payload_layout) {
    using namespace psvp::proto::regist_layout;
    for (const ConsoleKind kind : {ConsoleKind::ps4, ConsoleKind::ps5}) {
        const auto request = psvp::proto::build_regist_request(kind, kAccount, kPin, kNonce);
        const std::string inner = "Client-Type: dabfa2ec873de5839bee8d3f4c0239c4282c07c25c6077a2931afcf0adc0d34f\r\n"
                                  "Np-AccountId: ASNFZ4mrze8=\r\n";
        CHECK(request.payload.size() == kInnerOffset + inner.size());

        // Filler everywhere except the two halves of the wrapped nonce.
        const Key16 wrapped = psvp::proto::wrap_nonce(kind, kNonce, kFiller >> 3).value();
        for (std::size_t i = 0; i < kInnerOffset; ++i) {
            if (i >= kWrappedLowAt && i < kWrappedLowAt + 8) CHECK(request.payload[i] == wrapped[i - kWrappedLowAt]);
            else if (i >= kWrappedHighAt && i < kWrappedHighAt + 8) CHECK(request.payload[i] == wrapped[8 + i - kWrappedHighAt]);
            else CHECK(request.payload[i] == kFiller);
        }

        // The rest opens, with the request's own cipher, to the client type and the account.
        const std::vector<std::uint8_t> sealed(request.payload.begin() + kInnerOffset, request.payload.end());
        const auto opened = psvp::proto::decrypt(request.cipher, 0, sealed);
        CHECK(std::string(opened.begin(), opened.end()) == inner);
        CHECK(request.cipher.key == psvp::proto::make_regist_cipher(kind, kNonce, kFiller & 0x1f, kPin).value().key);
    }
}

// The encrypted part as the reference implementation produces it for the same inputs.
PSVP_TEST(regist_request_matches_the_reference) {
    using namespace psvp::test::vectors;
    using psvp::proto::regist_layout::kInnerOffset;
    const auto ps5 = psvp::proto::build_regist_request(ConsoleKind::ps5, kAccount, kPin, kNonce);
    const auto ps4 = psvp::proto::build_regist_request(ConsoleKind::ps4, kAccount, kPin, kNonce);
    CHECK(psvp::proto::hex_encode(std::span(ps5.payload).subspan(kInnerOffset)) == k_ps5_inner);
    CHECK(psvp::proto::hex_encode(std::span(ps4.payload).subspan(kInnerOffset)) == k_ps4_inner);
}

PSVP_TEST(regist_response_head_gives_the_body_length) {
    CHECK(psvp::proto::parse_regist_response_head("HTTP/1.1 200 OK\r\nContent-Length: 208\r\n\r\n") == 208U);
    CHECK(psvp::proto::parse_regist_response_head("HTTP/1.1 200 OK\nContent-Length:1\n\n") == 1U);
    // As a real PS5 writes it: the number padded with spaces.
    CHECK(psvp::proto::parse_regist_response_head("HTTP/1.1 200 OK\r\nConnection: close\r\nRP-Feature: 3\r\nContent-Length:        195\r\n\r\n") == 195U);
}

PSVP_TEST(regist_response_head_reports_a_refusal) {
    const auto refused =
        psvp::proto::parse_regist_response_head("HTTP/1.1 403 Forbidden\r\nRP-Application-Reason: 80108b09\r\n\r\n");
    CHECK(!refused.has_value());
    if (refused) return;
    CHECK(refused.error().failure == RegistFailure::refused);
    CHECK(refused.error().status == 403);
    CHECK(refused.error().reason == 0x80108b09U);
    CHECK(psvp::proto::describe(refused.error()).find("80108b09") != std::string::npos);

    const auto bare = psvp::proto::parse_regist_response_head("HTTP/1.1 500 Error\r\n\r\n");
    CHECK(!bare.has_value() && bare.error().failure == RegistFailure::refused && bare.error().reason == 0);
}

PSVP_TEST(regist_response_head_refuses_nonsense) {
    const auto failure_of = [](std::string_view head) {
        const auto parsed = psvp::proto::parse_regist_response_head(head);
        return parsed ? RegistFailure::incomplete : parsed.error().failure;
    };
    CHECK(failure_of("") == RegistFailure::malformed);
    CHECK(failure_of("not a response") == RegistFailure::malformed);
    CHECK(failure_of("HTTP/1.1 200 OK\r\n\r\n") == RegistFailure::malformed);
    CHECK(failure_of("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n") == RegistFailure::malformed);
    CHECK(failure_of("HTTP/1.1 200 OK\r\nContent-Length: many\r\n\r\n") == RegistFailure::malformed);
    CHECK(failure_of("HTTP/1.1 200 OK\r\nContent-Length: 4097\r\n\r\n") == RegistFailure::malformed);
    CHECK(failure_of("HTTP/1.1 200 OK\r\nContent-Length: -5\r\n\r\n") == RegistFailure::malformed);
}

PSVP_TEST(regist_response_body_gives_the_registration) {
    const auto cipher = psvp::proto::make_regist_cipher(ConsoleKind::ps5, kNonce, 1, kPin).value();
    const auto console = psvp::proto::parse_regist_response_body(cipher, sealed_body(cipher, kPs5Body));
    CHECK(console.has_value());
    if (!console) return;
    CHECK(console->kind == ConsoleKind::ps5);
    CHECK(console->nickname == "Living Room");
    CHECK(psvp::proto::hex_encode(console->mac) == "0123456789ab");
    CHECK(console->regist_key == "abcd1234");
    CHECK(psvp::proto::hex_encode(console->key) == "000102030405060708090a0b0c0d0e0f");
    CHECK(console->key_type == 2);
    CHECK(console->ap_ssid == "PS5-123");
    CHECK(console->ap_bssid == "aabbccddeeff");
    CHECK(console->ap_key == "secretkey");
    CHECK(console->ap_name == "PS5");
}

PSVP_TEST(regist_response_body_uses_the_family_names) {
    const auto cipher = psvp::proto::make_regist_cipher(ConsoleKind::ps4, kNonce, 1, kPin).value();
    const std::string ps4_body = "PS4-Mac: 0123456789ab\r\nPS4-RegistKey: 3132333435363738\r\nPS4-Nickname: Den\r\n"
                                 "RP-KeyType: 2\r\nRP-Key: 000102030405060708090a0b0c0d0e0f\r\n";
    const auto console = psvp::proto::parse_regist_response_body(cipher, sealed_body(cipher, ps4_body));
    CHECK(console.has_value() && console->nickname == "Den" && console->regist_key == "12345678");
    // A PS5's names mean nothing to a PS4 registration.
    const auto wrong = psvp::proto::parse_regist_response_body(cipher, sealed_body(cipher, kPs5Body));
    CHECK(!wrong.has_value() && wrong.error().failure == RegistFailure::incomplete);
}

PSVP_TEST(regist_response_body_stops_the_key_at_a_nul) {
    const auto cipher = psvp::proto::make_regist_cipher(ConsoleKind::ps5, kNonce, 1, kPin).value();
    const std::string body = "PS5-Mac: 0123456789ab\r\nPS5-RegistKey: 61626364000000000000000000000000\r\n"
                             "RP-Key: 000102030405060708090a0b0c0d0e0f\r\n";
    const auto console = psvp::proto::parse_regist_response_body(cipher, sealed_body(cipher, body));
    CHECK(console.has_value() && console->regist_key == "abcd");
}

PSVP_TEST(regist_response_body_needs_key_secret_and_address) {
    const auto cipher = psvp::proto::make_regist_cipher(ConsoleKind::ps5, kNonce, 1, kPin).value();
    const auto failure_of = [&](std::string_view body) {
        const auto parsed = psvp::proto::parse_regist_response_body(cipher, sealed_body(cipher, body));
        return parsed ? RegistFailure::refused : parsed.error().failure;
    };
    const std::string mac = "PS5-Mac: 0123456789ab\r\n";
    const std::string regist_key = "PS5-RegistKey: 6162636431323334\r\n";
    const std::string key = "RP-Key: 000102030405060708090a0b0c0d0e0f\r\n";
    CHECK(failure_of(mac + regist_key) == RegistFailure::incomplete);
    CHECK(failure_of(mac + key) == RegistFailure::incomplete);
    CHECK(failure_of(regist_key + key) == RegistFailure::incomplete);
    CHECK(failure_of(mac + regist_key + "RP-Key: 0001\r\n") == RegistFailure::incomplete);
    CHECK(failure_of("PS5-Mac: 01234567\r\n" + regist_key + key) == RegistFailure::incomplete);
    CHECK(failure_of(mac + "PS5-RegistKey: zz\r\n" + key) == RegistFailure::incomplete);
    CHECK(failure_of(mac + "PS5-RegistKey: 0000\r\n" + key) == RegistFailure::incomplete);
    CHECK(failure_of(mac + "PS5-RegistKey: 6162636431323334616263643132333461\r\n" + key) == RegistFailure::incomplete);
}

// The wrong PIN gives the wrong key: what comes out is noise, never a registration.
PSVP_TEST(regist_response_body_does_not_open_with_the_wrong_pin) {
    const auto right = psvp::proto::make_regist_cipher(ConsoleKind::ps5, kNonce, 1, kPin).value();
    const auto wrong = psvp::proto::make_regist_cipher(ConsoleKind::ps5, kNonce, 1, kPin + 1).value();
    CHECK(!psvp::proto::parse_regist_response_body(wrong, sealed_body(right, kPs5Body)).has_value());
}

PSVP_TEST(regist_response_body_ignores_text_fields_that_are_too_long) {
    const auto cipher = psvp::proto::make_regist_cipher(ConsoleKind::ps5, kNonce, 1, kPin).value();
    const std::string body = "PS5-Nickname: " + std::string(0x20, 'n') + "\r\nAP-Ssid: " + std::string(0x2f, 's') + "\r\n" + kPs5Body;
    const auto console = psvp::proto::parse_regist_response_body(cipher, sealed_body(cipher, body));
    CHECK(console.has_value());
    if (!console) return;
    CHECK(console->nickname == "Living Room");          // the long one was skipped, the next one kept
    CHECK(console->ap_ssid == "PS5-123");               // a later value replaces an earlier one that fit
}
