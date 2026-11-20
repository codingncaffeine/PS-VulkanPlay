#include <cstdint>
#include <vector>

#include "check.h"
#include "proto/erasure.h"

using namespace psvp::proto;

// The field: x^8 + x^4 + x^3 + x^2 + 1, so 2 * 0x80 = 0x100 ^ 0x11d = 0x1d.
PSVP_TEST(erasure_field_arithmetic) {
    CHECK(gf256_multiply(2, 0x80) == 0x1d);
    CHECK(gf256_multiply(0, 0x53) == 0 && gf256_multiply(0x53, 0) == 0);
    CHECK(gf256_multiply(1, 0x53) == 0x53);
    CHECK(gf256_multiply(2, 3) == 6);          // x * (x + 1) = x^2 + x
    CHECK(gf256_multiply(0x1d, 0x1d) == 0x4c); // 2^8 squared = 2^16
    for (unsigned a = 1; a < 256; ++a) {
        const auto value = static_cast<std::uint8_t>(a);
        CHECK(gf256_multiply(value, gf256_inverse(value)) == 1);
    }
    // The factor of source unit j in repair unit i, of m: 1 / (i xor (m + j)).
    CHECK(repair_factor(0, 0, 1) == 1);                       // 1 / 1
    CHECK(repair_factor(0, 1, 1) == gf256_inverse(2));
    CHECK(repair_factor(1, 0, 2) == gf256_inverse(3));        // 1 / (1 xor 2)
}

// One repair unit of one: every factor is 1 / (0 xor (1 + j)).
PSVP_TEST(erasure_repairs_one_lost_unit) {
    // Three source units of two bytes and one repair unit, four bytes apart.
    std::vector<std::uint8_t> frame{0x11, 0x22, 0, 0, 0x33, 0x44, 0, 0, 0x55, 0x66, 0, 0, 0, 0, 0, 0};
    CHECK(make_repair_units(frame, 2, 4, 3, 1));
    const auto expected = [&](std::size_t byte) {
        return static_cast<std::uint8_t>(gf256_multiply(frame[byte], gf256_inverse(1)) ^ gf256_multiply(frame[4 + byte], gf256_inverse(2)) ^
                                         gf256_multiply(frame[8 + byte], gf256_inverse(3)));
    };
    CHECK(frame[12] == expected(0) && frame[13] == expected(1));
    CHECK(frame[14] == 0 && frame[15] == 0);  // nothing is written between units
    const std::vector<std::uint8_t> whole = frame;
    for (unsigned lost = 0; lost < 3; ++lost) {
        std::vector<std::uint8_t> damaged = whole;
        damaged[lost * 4] = 0xee;
        damaged[lost * 4 + 1] = 0xee;
        const unsigned missing[] = {lost};
        CHECK(repair_units(damaged, 2, 4, 3, 1, missing));
        CHECK(damaged == whole);
    }
    // The repair unit alone missing: nothing to restore.
    std::vector<std::uint8_t> damaged = whole;
    const unsigned repair_missing[] = {3};
    CHECK(repair_units(damaged, 2, 4, 3, 1, repair_missing) && damaged == whole);
    // Two missing with one repair unit: refused, and the frame is left as it was.
    damaged[0] = 0xee;
    const std::vector<std::uint8_t> before = damaged;
    const unsigned two[] = {0, 1};
    CHECK(!repair_units(damaged, 2, 4, 3, 1, two) && damaged == before);
}

PSVP_TEST(erasure_refuses_what_does_not_add_up) {
    std::vector<std::uint8_t> frame(64, 1);
    const unsigned none[] = {0};
    CHECK(!repair_units(frame, 0, 4, 3, 1, {}));                 // empty units
    CHECK(!repair_units(frame, 8, 4, 3, 1, {}));                 // units overlap
    CHECK(!repair_units(frame, 4, 4, 0, 1, {}));                 // no source units
    CHECK(!repair_units(frame, 4, 4, 15, 2, {}));                // the frame is shorter than its units
    CHECK(repair_units(frame, 4, 4, 15, 1, {}));                 // exactly as long
    const unsigned out_of_range[] = {16};
    CHECK(!repair_units(frame, 4, 4, 15, 1, out_of_range));
    const unsigned twice[] = {1, 1};
    CHECK(!repair_units(frame, 4, 4, 14, 2, twice));
    CHECK(!make_repair_units(frame, 4, 4, 15, 2));
    (void)none;
    // More units than the field has elements.
    std::vector<std::uint8_t> large(300, 0);
    CHECK(!make_repair_units(large, 1, 1, 200, 57));
    CHECK(make_repair_units(large, 1, 1, 200, 56));
}
