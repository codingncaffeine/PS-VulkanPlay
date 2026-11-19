#include <cstdint>
#include <vector>

#include "check.h"
#include "proto/hex.h"

using psvp::proto::hex_decode;
using psvp::proto::hex_encode;

PSVP_TEST(hex_encodes_in_lowercase) {
    const std::vector<std::uint8_t> bytes{0x00, 0x01, 0x7f, 0x80, 0xab, 0xff};
    CHECK(hex_encode(bytes) == "00017f80abff");
    CHECK(hex_encode({}) == "");
}

PSVP_TEST(hex_decodes_both_cases) {
    const std::vector<std::uint8_t> bytes{0x00, 0x01, 0x7f, 0x80, 0xab, 0xff};
    CHECK(hex_decode("00017f80abff") == bytes);
    CHECK(hex_decode("00017F80ABFF") == bytes);
    CHECK(hex_decode("") == std::vector<std::uint8_t>{});
}

PSVP_TEST(hex_refuses_what_is_not_hex) {
    CHECK(!hex_decode("0").has_value());
    CHECK(!hex_decode("abc").has_value());
    CHECK(!hex_decode("0g").has_value());
    CHECK(!hex_decode("0x12").has_value());
    CHECK(!hex_decode(" 12 ").has_value());
    CHECK(!hex_decode("-1").has_value());
}

PSVP_TEST(hex_round_trips_every_byte) {
    std::vector<std::uint8_t> bytes;
    for (int value = 0; value < 256; ++value) bytes.push_back(static_cast<std::uint8_t>(value));
    CHECK(hex_decode(hex_encode(bytes)) == bytes);
}
