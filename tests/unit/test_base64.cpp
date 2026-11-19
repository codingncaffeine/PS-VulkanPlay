#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "check.h"
#include "proto/base64.h"

using psvp::proto::base64_decode;
using psvp::proto::base64_encode;
using psvp::proto::Base64Error;

namespace {

std::vector<std::uint8_t> bytes_of(std::string_view text) { return {text.begin(), text.end()}; }

bool refused_as(std::string_view text, Base64Error expected) {
    const auto result = base64_decode(text);
    return !result.has_value() && result.error() == expected;
}

}  // namespace

// The test vectors of RFC 4648, section 10.
PSVP_TEST(base64_encodes_the_rfc_vectors) {
    CHECK(base64_encode(bytes_of("")) == "");
    CHECK(base64_encode(bytes_of("f")) == "Zg==");
    CHECK(base64_encode(bytes_of("fo")) == "Zm8=");
    CHECK(base64_encode(bytes_of("foo")) == "Zm9v");
    CHECK(base64_encode(bytes_of("foob")) == "Zm9vYg==");
    CHECK(base64_encode(bytes_of("fooba")) == "Zm9vYmE=");
    CHECK(base64_encode(bytes_of("foobar")) == "Zm9vYmFy");
}

PSVP_TEST(base64_decodes_the_rfc_vectors) {
    CHECK(base64_decode("") == bytes_of(""));
    CHECK(base64_decode("Zg==") == bytes_of("f"));
    CHECK(base64_decode("Zm8=") == bytes_of("fo"));
    CHECK(base64_decode("Zm9v") == bytes_of("foo"));
    CHECK(base64_decode("Zm9vYg==") == bytes_of("foob"));
    CHECK(base64_decode("Zm9vYmE=") == bytes_of("fooba"));
    CHECK(base64_decode("Zm9vYmFy") == bytes_of("foobar"));
}

PSVP_TEST(base64_uses_both_high_alphabet_characters) {
    const std::vector<std::uint8_t> bytes{0xfb, 0xff, 0xbf};
    CHECK(base64_encode(bytes) == "+/+/");
    CHECK(base64_decode("+/+/") == bytes);
}

PSVP_TEST(base64_round_trips_every_length_and_byte) {
    std::vector<std::uint8_t> bytes;
    for (int length = 0; length <= 300; ++length) {
        const auto back = base64_decode(base64_encode(bytes));
        CHECK(back.has_value() && *back == bytes);
        bytes.push_back(static_cast<std::uint8_t>((length * 37 + 11) & 0xff));
    }
    for (int value = 0; value < 256; ++value) {
        const std::vector<std::uint8_t> one{static_cast<std::uint8_t>(value)};
        CHECK(base64_decode(base64_encode(one)) == one);
    }
}

// An account ID is eight bytes: twelve characters, the last one padding.
PSVP_TEST(base64_of_eight_bytes_is_twelve_characters) {
    const std::vector<std::uint8_t> id{0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
    const std::string text = base64_encode(id);
    CHECK(text == "ASNFZ4mrze8=");
    CHECK(base64_decode(text) == id);
}

PSVP_TEST(base64_refuses_a_wrong_length) {
    CHECK(refused_as("Z", Base64Error::bad_length));
    CHECK(refused_as("Zg=", Base64Error::bad_length));
    CHECK(refused_as("Zm9vY", Base64Error::bad_length));
    CHECK(refused_as("Zm9v\n", Base64Error::bad_length));
}

PSVP_TEST(base64_refuses_characters_outside_the_alphabet) {
    CHECK(refused_as("Zm 9", Base64Error::bad_character));
    CHECK(refused_as("Zm9-", Base64Error::bad_character));
    CHECK(refused_as("Zm9_", Base64Error::bad_character));
    CHECK(refused_as("\xff\xff\xff\xff", Base64Error::bad_character));
    CHECK(refused_as(std::string_view("Zm\0v", 4), Base64Error::bad_character));
}

PSVP_TEST(base64_refuses_padding_in_the_wrong_place) {
    CHECK(refused_as("=m9v", Base64Error::bad_padding));
    CHECK(refused_as("Zm=v", Base64Error::bad_padding));
    CHECK(refused_as("Z===", Base64Error::bad_padding));
    CHECK(refused_as("====", Base64Error::bad_padding));
    CHECK(refused_as("Zg==Zg==", Base64Error::bad_padding));
}

PSVP_TEST(base64_refuses_loose_bits_before_the_padding) {
    CHECK(refused_as("Zh==", Base64Error::non_canonical));
    CHECK(refused_as("Zm9=", Base64Error::non_canonical));
}

PSVP_TEST(base64_names_every_error) {
    for (const Base64Error error : {Base64Error::bad_length, Base64Error::bad_character, Base64Error::bad_padding,
                                    Base64Error::non_canonical}) {
        CHECK(!psvp::proto::to_string(error).empty());
        CHECK(psvp::proto::to_string(error) != "unknown error");
    }
}
