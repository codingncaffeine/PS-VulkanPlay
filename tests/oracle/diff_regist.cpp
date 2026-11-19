// Registration: our cipher and our request against the reference's.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <chiaki/regist.h>
#include <chiaki/rpcrypt.h>

#include "oracle.h"
#include "proto/hex.h"
#include "proto/regist.h"
#include "proto/rp_cipher.h"

namespace psvp::oracle {
namespace {

using proto::ConsoleKind;
using proto::Key16;

ChiakiTarget target_of(ConsoleKind kind) {
    return kind == ConsoleKind::ps5 ? CHIAKI_TARGET_PS5_1 : CHIAKI_TARGET_PS4_10;
}

Key16 random_key(Random& random) {
    Key16 key{};
    for (auto& byte : key) byte = static_cast<std::uint8_t>(random.below(256));
    return key;
}

bool same(const Key16& ours, const std::uint8_t* theirs) { return std::memcmp(ours.data(), theirs, ours.size()) == 0; }

std::vector<std::uint8_t> reference_payload(ConsoleKind kind, const Key16& nonce, const proto::AccountId& account,
                                            std::uint32_t pin, ChiakiRPCrypt& crypt) {
    std::vector<std::uint8_t> buffer(0x400);
    std::size_t size = buffer.size();
    if (chiaki_regist_request_payload_format(target_of(kind), nonce.data(), buffer.data(), &size, &crypt, nullptr,
                                             account.data(), pin, nullptr) != CHIAKI_ERR_SUCCESS) {
        return {};
    }
    buffer.resize(size);
    return buffer;
}

}  // namespace

Outcome diff_regist() {
    Outcome outcome;
    Random random(0x70737670'72656721ULL);
    const auto mismatch = [&](const char* what) {
        if (++outcome.mismatches <= 5) std::fprintf(stderr, "    MISMATCH regist %s\n", what);
    };
    const auto compare = [&](bool equal, const char* what) {
        ++outcome.compared;
        if (!equal) mismatch(what);
    };

    for (int round = 0; round < 10000; ++round) {
        const ConsoleKind kind = (round & 1) != 0 ? ConsoleKind::ps5 : ConsoleKind::ps4;
        const Key16 nonce = random_key(random);
        const std::uint32_t pin = random.below(100000000);
        const std::size_t key_offset = random.below(32);
        const std::size_t wrap_offset = random.below(32);

        // The cipher of a registration: the same key and nonce.
        ChiakiRPCrypt reference{};
        const bool reference_ok = chiaki_rpcrypt_init_regist(&reference, target_of(kind), nonce.data(), key_offset,
                                                             pin) == CHIAKI_ERR_SUCCESS;
        const auto ours = proto::make_regist_cipher(kind, nonce, key_offset, pin);
        compare(reference_ok && ours && same(ours->key, reference.bright) && same(ours->nonce, reference.ambassador),
                "cipher key");
        if (!ours || !reference_ok) continue;

        // The wrapped nonce, and our way back from it.
        std::uint8_t wrapped_reference[16];
        chiaki_rpcrypt_aeropause(target_of(kind), wrap_offset, wrapped_reference, nonce.data());
        const auto wrapped = proto::wrap_nonce(kind, nonce, wrap_offset);
        compare(wrapped && same(*wrapped, wrapped_reference), "wrapped nonce");
        if (wrapped) compare(proto::unwrap_nonce(kind, *wrapped, wrap_offset) == nonce, "unwrapped nonce");

        // The starting vector of a message.
        const std::uint64_t counter = (round % 4 == 0) ? random.below(4) : random.next();
        std::uint8_t iv_reference[16];
        chiaki_rpcrypt_generate_iv(&reference, iv_reference, counter);
        compare(same(proto::cipher_iv(*ours, counter), iv_reference), "starting vector");

        // Both directions of the cipher, on text of any length.
        std::vector<std::uint8_t> text(random.below(300));
        for (auto& byte : text) byte = static_cast<std::uint8_t>(random.below(256));
        std::vector<std::uint8_t> sealed_reference(text.size());
        std::vector<std::uint8_t> opened_reference(text.size());
        chiaki_rpcrypt_encrypt(&reference, counter, text.data(), sealed_reference.data(), text.size());
        chiaki_rpcrypt_decrypt(&reference, counter, text.data(), opened_reference.data(), text.size());
        compare(proto::encrypt(*ours, counter, text) == sealed_reference, "encrypt");
        compare(proto::decrypt(*ours, counter, text) == opened_reference, "decrypt");

        // The whole request payload for the same nonce, account and PIN, byte for byte.
        if (round % 10 == 0) {
            proto::AccountId account{};
            for (auto& byte : account) byte = static_cast<std::uint8_t>(random.below(256));
            ChiakiRPCrypt payload_crypt{};
            const std::vector<std::uint8_t> payload_reference = reference_payload(kind, nonce, account, pin, payload_crypt);
            const proto::RegistRequest request = proto::build_regist_request(kind, account, pin, nonce);
            compare(!payload_reference.empty() && request.payload == payload_reference, "request payload");
            compare(same(request.cipher.key, payload_crypt.bright) && same(request.cipher.nonce, payload_crypt.ambassador),
                    "request cipher");
        }
    }
    // The cipher of a session, from the console's nonce and the shared secret.
    int session_rounds = 0;
    for (int round = 0; round < 20000; ++round) {
        const ConsoleKind kind = (round & 1) != 0 ? ConsoleKind::ps5 : ConsoleKind::ps4;
        const Key16 console_nonce = random_key(random);
        const Key16 secret = random_key(random);
        ChiakiRPCrypt reference{};
        chiaki_rpcrypt_init_auth(&reference, target_of(kind), console_nonce.data(), secret.data());
        const proto::RpCipher ours = proto::make_session_cipher(kind, console_nonce, secret);
        compare(same(ours.key, reference.bright) && same(ours.nonce, reference.ambassador), "session cipher");
        ++session_rounds;
    }
    std::fprintf(stderr, "  regist: %d comparisons, %d mismatches (%d of them session ciphers)\n", outcome.compared,
                 outcome.mismatches, session_rounds);
    return outcome;
}

void print_regist_vectors() {
    const Key16 nonce{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    const proto::AccountId account{0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
    const std::uint32_t pin = 12345678;
    const std::string text = "The quick brown fox jumps over the lazy dog";
    const auto hex = [](const std::uint8_t* data, std::size_t size) {
        return proto::hex_encode(std::span<const std::uint8_t>(data, size));
    };
    for (const ConsoleKind kind : {ConsoleKind::ps4, ConsoleKind::ps5}) {
        const char* name = kind == ConsoleKind::ps5 ? "ps5" : "ps4";
        ChiakiRPCrypt crypt{};
        chiaki_rpcrypt_init_regist(&crypt, target_of(kind), nonce.data(), 1, pin);
        std::uint8_t wrapped[16];
        chiaki_rpcrypt_aeropause(target_of(kind), 8, wrapped, nonce.data());
        std::uint8_t iv0[16];
        std::uint8_t iv7[16];
        chiaki_rpcrypt_generate_iv(&crypt, iv0, 0);
        chiaki_rpcrypt_generate_iv(&crypt, iv7, 7);
        std::vector<std::uint8_t> sealed(text.size());
        chiaki_rpcrypt_encrypt(&crypt, 0, reinterpret_cast<const std::uint8_t*>(text.data()), sealed.data(), text.size());
        ChiakiRPCrypt payload_crypt{};
        const std::vector<std::uint8_t> payload = reference_payload(kind, nonce, account, pin, payload_crypt);
        std::printf("// %s, computed by the reference\n", name);
        std::printf("constexpr std::string_view k_%s_key = \"%s\";\n", name, hex(crypt.bright, 16).c_str());
        std::printf("constexpr std::string_view k_%s_wrapped = \"%s\";\n", name, hex(wrapped, 16).c_str());
        std::printf("constexpr std::string_view k_%s_iv0 = \"%s\";\n", name, hex(iv0, 16).c_str());
        std::printf("constexpr std::string_view k_%s_iv7 = \"%s\";\n", name, hex(iv7, 16).c_str());
        std::printf("constexpr std::string_view k_%s_sealed = \"%s\";\n", name, hex(sealed.data(), sealed.size()).c_str());
        std::printf("constexpr std::string_view k_%s_inner = \"%s\";\n", name,
                    hex(payload.data() + 0x1e0, payload.size() - 0x1e0).c_str());
        // The session cipher for the console nonce 00 11 .. ff and the secret a0 a1 .. af.
        const Key16 secret{0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf};
        ChiakiRPCrypt session{};
        chiaki_rpcrypt_init_auth(&session, target_of(kind), nonce.data(), secret.data());
        std::printf("constexpr std::string_view k_%s_session_key = \"%s\";\n", name, hex(session.bright, 16).c_str());
        std::printf("constexpr std::string_view k_%s_session_nonce = \"%s\";\n", name, hex(session.ambassador, 16).c_str());
    }
}

}  // namespace psvp::oracle
