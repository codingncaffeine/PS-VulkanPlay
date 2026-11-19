#include <string>
#include <string_view>

#include "check.h"
#include "proto/text_message.h"

using psvp::proto::parse_header_block;
using psvp::proto::parse_status_message;
using psvp::proto::TextError;

PSVP_TEST(text_reads_a_status_and_its_headers) {
    const auto message = parse_status_message("HTTP/1.1 200 Ok\nhost-id:AABBCCDDEEFF\nhost-name:Living Room\n");
    CHECK(message.has_value());
    if (!message) return;
    CHECK(message->status == 200);
    CHECK(message->headers.size() == 2);
    CHECK(message->find("host-id") == "AABBCCDDEEFF");
    CHECK(message->find("host-name") == "Living Room");
    CHECK(!message->find("host-type").has_value());
}

PSVP_TEST(text_accepts_both_line_endings) {
    const auto message = parse_status_message("HTTP/1.1 620 Server Standby\r\nhost-id:AA\r\nhost-type:PS5\r\n\r\n");
    CHECK(message.has_value());
    if (!message) return;
    CHECK(message->status == 620);
    CHECK(message->headers.size() == 2);
    CHECK(message->find("host-type") == "PS5");
}

PSVP_TEST(text_drops_one_space_after_the_colon) {
    const auto message = parse_header_block("Content-Length: 80\r\nPadded:  two\r\nTight:none\r\n");
    CHECK(message.has_value());
    if (!message) return;
    CHECK(message->status == 0);
    CHECK(message->find("Content-Length") == "80");
    CHECK(message->find("Padded") == " two");
    CHECK(message->find("Tight") == "none");
}

PSVP_TEST(text_names_are_case_sensitive_and_the_first_wins) {
    const auto message = parse_header_block("Key:first\nkey:lower\nKey:second\n");
    CHECK(message.has_value());
    if (!message) return;
    CHECK(message->find("Key") == "first");
    CHECK(message->find("key") == "lower");
    CHECK(!message->find("KEY").has_value());
}

PSVP_TEST(text_keeps_an_empty_value) {
    const auto message = parse_header_block("running-app-name:\nhost-name:Den\n");
    CHECK(message.has_value());
    if (!message) return;
    CHECK(message->headers.size() == 2);
    CHECK(message->find("running-app-name") == "");
    CHECK(message->find("host-name") == "Den");
}

// A line that never ended may have lost its tail on the way: neither a cut
// value nor a cut name is reported.
PSVP_TEST(text_does_not_read_a_last_line_without_an_end) {
    const auto cut_value = parse_header_block("host-id:AA\nhost-name:Living Ro");
    CHECK(cut_value.has_value() && cut_value->headers.size() == 1 && !cut_value->find("host-name").has_value());
    const auto cut_name = parse_header_block("host-id:AA\nhost-na");
    CHECK(cut_name.has_value() && cut_name->headers.size() == 1);
}

PSVP_TEST(text_stops_at_a_nul_byte) {
    const std::string datagram("HTTP/1.1 200 Ok\nhost-id:AA\n\0host-name:hidden\n", 44);
    const auto message = parse_status_message(datagram);
    CHECK(message.has_value());
    if (!message) return;
    CHECK(message->headers.size() == 1);
    CHECK(!message->find("host-name").has_value());
}

PSVP_TEST(text_refuses_a_line_that_is_not_a_header) {
    CHECK(parse_header_block("no colon here\n").error() == TextError::bad_header);
    CHECK(parse_header_block(":value without a name\n").error() == TextError::bad_header);
    CHECK(parse_status_message("HTTP/1.1 200 Ok\nhost-id:AA\ngarbage\n").error() == TextError::bad_header);
}

PSVP_TEST(text_refuses_a_bad_status_line) {
    CHECK(parse_status_message("").error() == TextError::no_status_line);
    CHECK(parse_status_message("HTTP/1.0 200 Ok\n").error() == TextError::no_status_line);
    CHECK(parse_status_message("host-id:AA\n").error() == TextError::no_status_line);
    CHECK(parse_status_message("HTTP/1.1 Ok\n").error() == TextError::bad_status);
    CHECK(parse_status_message("HTTP/1.1 000 Ok\n").error() == TextError::bad_status);
    CHECK(parse_status_message("HTTP/1.1 -200 Ok\n").error() == TextError::bad_status);
    CHECK(parse_status_message("HTTP/1.1 20000 Ok\n").error() == TextError::bad_status);
}

PSVP_TEST(text_reads_a_status_line_alone) {
    const auto message = parse_status_message("HTTP/1.1 403 Forbidden");
    CHECK(message.has_value() && message->status == 403 && message->headers.empty());
}

PSVP_TEST(text_limits_the_number_of_headers) {
    std::string many;
    for (std::size_t i = 0; i < psvp::proto::kMaxTextHeaders; ++i) many += "k:v\n";
    CHECK(parse_header_block(many).has_value());
    many += "k:v\n";
    CHECK(parse_header_block(many).error() == TextError::too_many_headers);
}

PSVP_TEST(text_names_every_error) {
    for (const TextError error : {TextError::no_status_line, TextError::bad_status, TextError::bad_header,
                                  TextError::too_many_headers}) {
        CHECK(psvp::proto::to_string(error) != "unknown error");
    }
}
