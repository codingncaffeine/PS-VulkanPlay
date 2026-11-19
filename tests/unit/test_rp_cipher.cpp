#include <cstdint>
#include <string_view>
#include <vector>

#include "check.h"
#include "proto/hex.h"
#include "proto/rp_cipher.h"
#include "vectors_regist.h"

using psvp::proto::ConsoleKind;
using psvp::proto::Key16;

namespace {

constexpr Key16 kNonce{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
constexpr std::uint32_t kPin = 12345678;
constexpr std::string_view kText = "The quick brown fox jumps over the lazy dog";

std::vector<std::uint8_t> bytes_of(std::string_view text) { return {text.begin(), text.end()}; }

std::string hex_of(const Key16& key) { return psvp::proto::hex_encode(key); }

}  // namespace

// The expected values were computed by the reference implementation
// (tools/oracle/run.sh --vectors) for the inputs above.
PSVP_TEST(cipher_matches_the_reference_on_ps5) {
    using namespace psvp::test::vectors;
    const auto cipher = psvp::proto::make_regist_cipher(ConsoleKind::ps5, kNonce, 1, kPin);
    CHECK(cipher.has_value());
    if (!cipher) return;
    CHECK(hex_of(cipher->key) == k_ps5_key);
    CHECK(hex_of(psvp::proto::wrap_nonce(ConsoleKind::ps5, kNonce, 8).value()) == k_ps5_wrapped);
    CHECK(hex_of(psvp::proto::cipher_iv(*cipher, 0)) == k_ps5_iv0);
    CHECK(hex_of(psvp::proto::cipher_iv(*cipher, 7)) == k_ps5_iv7);
    CHECK(psvp::proto::hex_encode(psvp::proto::encrypt(*cipher, 0, bytes_of(kText))) == k_ps5_sealed);
}

PSVP_TEST(cipher_matches_the_reference_on_ps4) {
    using namespace psvp::test::vectors;
    const auto cipher = psvp::proto::make_regist_cipher(ConsoleKind::ps4, kNonce, 1, kPin);
    CHECK(cipher.has_value());
    if (!cipher) return;
    CHECK(hex_of(cipher->key) == k_ps4_key);
    CHECK(hex_of(psvp::proto::wrap_nonce(ConsoleKind::ps4, kNonce, 8).value()) == k_ps4_wrapped);
    CHECK(hex_of(psvp::proto::cipher_iv(*cipher, 0)) == k_ps4_iv0);
    CHECK(hex_of(psvp::proto::cipher_iv(*cipher, 7)) == k_ps4_iv7);
    CHECK(psvp::proto::hex_encode(psvp::proto::encrypt(*cipher, 0, bytes_of(kText))) == k_ps4_sealed);
}

PSVP_TEST(cipher_refuses_an_offset_outside_the_tables) {
    CHECK(psvp::proto::make_regist_cipher(ConsoleKind::ps5, kNonce, 31, kPin).has_value());
    CHECK(!psvp::proto::make_regist_cipher(ConsoleKind::ps5, kNonce, 32, kPin).has_value());
    CHECK(psvp::proto::wrap_nonce(ConsoleKind::ps4, kNonce, 31).has_value());
    CHECK(!psvp::proto::wrap_nonce(ConsoleKind::ps4, kNonce, 32).has_value());
    CHECK(!psvp::proto::unwrap_nonce(ConsoleKind::ps4, kNonce, 32).has_value());
}

PSVP_TEST(cipher_pin_changes_only_the_last_four_key_bytes) {
    for (const ConsoleKind kind : {ConsoleKind::ps4, ConsoleKind::ps5}) {
        const Key16 without = psvp::proto::make_regist_cipher(kind, kNonce, 5, 0).value().key;
        const Key16 with = psvp::proto::make_regist_cipher(kind, kNonce, 5, 0x01020304).value().key;
        for (std::size_t i = 0; i < 12; ++i) CHECK(without[i] == with[i]);
        CHECK((without[12] ^ with[12]) == 0x01);
        CHECK((without[13] ^ with[13]) == 0x02);
        CHECK((without[14] ^ with[14]) == 0x03);
        CHECK((without[15] ^ with[15]) == 0x04);
    }
}

PSVP_TEST(cipher_unwraps_what_it_wraps_at_every_offset) {
    for (const ConsoleKind kind : {ConsoleKind::ps4, ConsoleKind::ps5}) {
        for (std::size_t offset = 0; offset < psvp::proto::kKeyOffsets; ++offset) {
            const Key16 wrapped = psvp::proto::wrap_nonce(kind, kNonce, offset).value();
            CHECK(wrapped != kNonce);
            CHECK(psvp::proto::unwrap_nonce(kind, wrapped, offset) == kNonce);
        }
    }
}

PSVP_TEST(cipher_opens_what_it_seals) {
    const auto cipher = psvp::proto::make_regist_cipher(ConsoleKind::ps5, kNonce, 3, kPin).value();
    for (const std::size_t length : {std::size_t{0}, std::size_t{1}, std::size_t{15}, std::size_t{16}, std::size_t{17},
                                     std::size_t{1000}}) {
        std::vector<std::uint8_t> plain(length);
        for (std::size_t i = 0; i < length; ++i) plain[i] = static_cast<std::uint8_t>(i * 7 + 3);
        const auto sealed = psvp::proto::encrypt(cipher, 42, plain);
        CHECK(sealed.size() == plain.size());
        if (length > 0) CHECK(sealed != plain);
        CHECK(psvp::proto::decrypt(cipher, 42, sealed) == plain);
        if (length > 0) CHECK(psvp::proto::decrypt(cipher, 43, sealed) != plain);
    }
}

PSVP_TEST(cipher_vector_depends_on_counter_nonce_and_family) {
    const auto ps5 = psvp::proto::make_regist_cipher(ConsoleKind::ps5, kNonce, 1, kPin).value();
    auto ps4 = ps5;
    ps4.kind = ConsoleKind::ps4;
    auto other_nonce = ps5;
    other_nonce.nonce[0] ^= 1;
    CHECK(psvp::proto::cipher_iv(ps5, 0) != psvp::proto::cipher_iv(ps5, 1));
    CHECK(psvp::proto::cipher_iv(ps5, 0) != psvp::proto::cipher_iv(ps4, 0));
    CHECK(psvp::proto::cipher_iv(ps5, 0) != psvp::proto::cipher_iv(other_nonce, 0));
    CHECK(psvp::proto::cipher_iv(ps5, 9) == psvp::proto::cipher_iv(ps5, 9));
}

PSVP_TEST(cipher_random_keys_differ) {
    CHECK(psvp::proto::random_key() != psvp::proto::random_key());
}
