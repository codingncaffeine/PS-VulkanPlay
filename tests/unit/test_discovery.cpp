#include <string>
#include <string_view>

#include "check.h"
#include "proto/discovery.h"

using psvp::proto::ConsoleKind;
using psvp::proto::ConsoleState;
using psvp::proto::parse_discovery_reply;
using psvp::proto::wake_credential;

namespace {

// The datagrams end with a NUL byte, which a literal has to be told to keep.
template <std::size_t N>
std::string with_nul(const char (&text)[N]) {
    return std::string(text, N);
}

}  // namespace

PSVP_TEST(discovery_search_packets_are_exact) {
    CHECK(psvp::proto::discovery_search_packet(ConsoleKind::ps5) ==
          with_nul("SRCH * HTTP/1.1\ndevice-discovery-protocol-version:00030010\n"));
    CHECK(psvp::proto::discovery_search_packet(ConsoleKind::ps4) ==
          with_nul("SRCH * HTTP/1.1\ndevice-discovery-protocol-version:00020020\n"));
}

PSVP_TEST(discovery_wake_packets_are_exact) {
    CHECK(psvp::proto::discovery_wake_packet(ConsoleKind::ps5, 305419896) ==
          with_nul("WAKEUP * HTTP/1.1\n"
                   "client-type:vr\n"
                   "auth-type:R\n"
                   "model:w\n"
                   "app-type:r\n"
                   "user-credential:305419896\n"
                   "device-discovery-protocol-version:00030010\n"));
    CHECK(psvp::proto::discovery_wake_packet(ConsoleKind::ps4, 18446744073709551615ULL) ==
          with_nul("WAKEUP * HTTP/1.1\n"
                   "client-type:vr\n"
                   "auth-type:R\n"
                   "model:w\n"
                   "app-type:r\n"
                   "user-credential:18446744073709551615\n"
                   "device-discovery-protocol-version:00020020\n"));
}

PSVP_TEST(discovery_ports_and_versions_per_family) {
    CHECK(psvp::proto::discovery_port(ConsoleKind::ps5) == 9302);
    CHECK(psvp::proto::discovery_port(ConsoleKind::ps4) == 987);
    CHECK(psvp::proto::rp_version(ConsoleKind::ps5) == "1.0");
    CHECK(psvp::proto::rp_version(ConsoleKind::ps4) == "10.0");
}

PSVP_TEST(discovery_reads_a_ps5_that_is_on) {
    const auto reply = parse_discovery_reply("HTTP/1.1 200 Ok\n"
                                             "host-id:AABBCCDDEEFF\n"
                                             "host-type:PS5\n"
                                             "host-name:Living Room\n"
                                             "host-request-port:997\n"
                                             "device-discovery-protocol-version:00030010\n"
                                             "system-version:08050001\n"
                                             "running-app-name:A Game\n"
                                             "running-app-titleid:PPSA01234\n");
    CHECK(reply.has_value());
    if (!reply) return;
    CHECK(reply->state == ConsoleState::ready);
    CHECK(reply->kind == ConsoleKind::ps5);
    CHECK(reply->host_id == "AABBCCDDEEFF");
    CHECK(reply->host_type == "PS5");
    CHECK(reply->host_name == "Living Room");
    CHECK(reply->request_port == 997);
    CHECK(reply->system_version == "08050001");
    CHECK(reply->running_app_name == "A Game");
    CHECK(reply->running_app_title_id == "PPSA01234");
    CHECK(reply->supported());
}

PSVP_TEST(discovery_reads_a_ps4_in_rest_mode) {
    const auto reply = parse_discovery_reply("HTTP/1.1 620 Server Standby\n"
                                             "host-id:001122334455\n"
                                             "host-type:PS4\n"
                                             "host-name:Den\n"
                                             "host-request-port:997\n"
                                             "device-discovery-protocol-version:00020020\n"
                                             "system-version:12020000\n");
    CHECK(reply.has_value());
    if (!reply) return;
    CHECK(reply->state == ConsoleState::standby);
    CHECK(reply->kind == ConsoleKind::ps4);
    CHECK(reply->running_app_name.empty());
    CHECK(reply->supported());
}

PSVP_TEST(discovery_marks_other_statuses_unknown) {
    const auto reply = parse_discovery_reply("HTTP/1.1 500 Error\nhost-id:AA\n");
    CHECK(reply.has_value() && reply->state == ConsoleState::unknown);
}

PSVP_TEST(discovery_keeps_only_a_valid_port) {
    const auto port_of = [](std::string_view value) {
        const std::string text = "HTTP/1.1 200 Ok\nhost-request-port:" + std::string(value) + "\n";
        return parse_discovery_reply(text).value().request_port;
    };
    CHECK(port_of("997") == 997);
    CHECK(port_of("65535") == 65535);
    CHECK(port_of("65536") == 0);
    CHECK(port_of("-1") == 0);
    CHECK(port_of("99x") == 0);
    CHECK(port_of("") == 0);
}

PSVP_TEST(discovery_knows_which_ps4_software_it_can_talk_to) {
    const auto supported = [](std::string_view version) {
        const std::string text = "HTTP/1.1 200 Ok\ndevice-discovery-protocol-version:00020020\nsystem-version:" +
                                 std::string(version) + "\n";
        return parse_discovery_reply(text).value().supported();
    };
    CHECK(supported("08000000"));
    CHECK(supported("12020000"));
    CHECK(!supported("07510000"));
    CHECK(!supported("06720001"));
    CHECK(!supported(""));
    CHECK(!supported("eight"));
}

PSVP_TEST(discovery_refuses_what_is_not_a_reply) {
    CHECK(!parse_discovery_reply("").has_value());
    CHECK(!parse_discovery_reply("SRCH * HTTP/1.1\n").has_value());
    CHECK(!parse_discovery_reply("HTTP/1.1 200 Ok\nthis line has no colon\n").has_value());
}

PSVP_TEST(discovery_wake_credential_is_the_key_read_as_hex) {
    CHECK(wake_credential("12345678") == 0x12345678ULL);
    CHECK(wake_credential("abcdef01") == 0xabcdef01ULL);
    CHECK(wake_credential("ABCDEF01") == 0xabcdef01ULL);
    CHECK(wake_credential("ffffffffffffffff") == 0xffffffffffffffffULL);
    CHECK(wake_credential("0") == 0ULL);
    CHECK(!wake_credential("").has_value());
    CHECK(!wake_credential("12345678901234567").has_value());
    CHECK(!wake_credential("1234567g").has_value());
    CHECK(!wake_credential(" 12345678").has_value());
    CHECK(!wake_credential("0x1234").has_value());
}

PSVP_TEST(discovery_names_every_state) {
    CHECK(psvp::proto::to_string(ConsoleState::ready) == "ready");
    CHECK(psvp::proto::to_string(ConsoleState::standby) == "standby");
    CHECK(psvp::proto::to_string(ConsoleState::unknown) == "unknown");
}
