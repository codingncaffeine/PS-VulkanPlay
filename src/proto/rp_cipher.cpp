#include "proto/rp_cipher.h"

#include <array>
#include <memory>
#include <stdexcept>

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace psvp::proto {
namespace {

#include "proto/key_tables.inc"

// The fixed tables are sixteen rows of kKeyOffsets choices each.
constexpr std::size_t kRowWidth = 0x20;

struct KindConstants {
    const std::array<std::uint8_t, 512>& regist_keys_0;
    const std::array<std::uint8_t, 512>& regist_keys_1;
    const Key16& iv_key;
    std::uint8_t wrap_bias;  // added, with the byte's position, when the nonce is wrapped
};

const KindConstants& constants(ConsoleKind kind) {
    static const KindConstants ps4{kRegistKeys0Ps4, kRegistKeys1Ps4, kIvKeyPs4, 0x29};
    static const KindConstants ps5{kRegistKeys0Ps5, kRegistKeys1Ps5, kIvKeyPs5, static_cast<std::uint8_t>(-0x2d)};
    return kind == ConsoleKind::ps5 ? ps5 : ps4;
}

struct CipherContextDeleter {
    void operator()(EVP_CIPHER_CTX* context) const noexcept { EVP_CIPHER_CTX_free(context); }
};

std::vector<std::uint8_t> run_cipher(const RpCipher& cipher, std::uint64_t counter, std::span<const std::uint8_t> in,
                                     bool encrypting) {
    const Key16 iv = cipher_iv(cipher, counter);
    const std::unique_ptr<EVP_CIPHER_CTX, CipherContextDeleter> context(EVP_CIPHER_CTX_new());
    if (!context) throw std::runtime_error("cipher: no context");
    if (EVP_CipherInit_ex(context.get(), EVP_aes_128_cfb128(), nullptr, cipher.key.data(), iv.data(),
                          encrypting ? 1 : 0) != 1) {
        throw std::runtime_error("cipher: cannot start AES-128-CFB");
    }
    std::vector<std::uint8_t> out(in.size());
    int written = 0;
    if (EVP_CipherUpdate(context.get(), out.data(), &written, in.data(), static_cast<int>(in.size())) != 1 ||
        static_cast<std::size_t>(written) != in.size()) {
        throw std::runtime_error("cipher: AES-128-CFB failed");
    }
    return out;
}

}  // namespace

std::optional<RpCipher> make_regist_cipher(ConsoleKind kind, const Key16& nonce, std::size_t key_offset,
                                           std::uint32_t pin) {
    if (key_offset >= kKeyOffsets) return std::nullopt;
    RpCipher cipher;
    cipher.kind = kind;
    cipher.nonce = nonce;
    const auto& table = constants(kind).regist_keys_0;
    for (std::size_t i = 0; i < cipher.key.size(); ++i) cipher.key[i] = table[i * kRowWidth + key_offset];
    // The PIN, most significant byte first, over the last four key bytes.
    cipher.key[12] ^= static_cast<std::uint8_t>(pin >> 24);
    cipher.key[13] ^= static_cast<std::uint8_t>(pin >> 16);
    cipher.key[14] ^= static_cast<std::uint8_t>(pin >> 8);
    cipher.key[15] ^= static_cast<std::uint8_t>(pin);
    return cipher;
}

std::optional<Key16> wrap_nonce(ConsoleKind kind, const Key16& nonce, std::size_t wrap_offset) {
    if (wrap_offset >= kKeyOffsets) return std::nullopt;
    const KindConstants& c = constants(kind);
    Key16 wrapped{};
    for (std::size_t i = 0; i < wrapped.size(); ++i) {
        const std::uint8_t mask = c.regist_keys_1[i * kRowWidth + wrap_offset];
        wrapped[i] = static_cast<std::uint8_t>((nonce[i] ^ mask) + c.wrap_bias + i);
    }
    return wrapped;
}

std::optional<Key16> unwrap_nonce(ConsoleKind kind, const Key16& wrapped, std::size_t wrap_offset) {
    if (wrap_offset >= kKeyOffsets) return std::nullopt;
    const KindConstants& c = constants(kind);
    Key16 nonce{};
    for (std::size_t i = 0; i < nonce.size(); ++i) {
        const std::uint8_t mask = c.regist_keys_1[i * kRowWidth + wrap_offset];
        nonce[i] = static_cast<std::uint8_t>(static_cast<std::uint8_t>(wrapped[i] - c.wrap_bias - i) ^ mask);
    }
    return nonce;
}

Key16 cipher_iv(const RpCipher& cipher, std::uint64_t counter) {
    // HMAC-SHA256 over the nonce and the counter (most significant byte first),
    // keyed with a constant of the console family; the first sixteen bytes are the vector.
    std::array<std::uint8_t, 24> message{};
    for (std::size_t i = 0; i < 16; ++i) message[i] = cipher.nonce[i];
    for (std::size_t i = 0; i < 8; ++i) message[16 + i] = static_cast<std::uint8_t>(counter >> (56 - 8 * i));

    const Key16& key = constants(cipher.kind).iv_key;
    std::array<std::uint8_t, 32> mac{};
    std::size_t mac_size = 0;
    if (EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA256", nullptr, key.data(), key.size(), message.data(),
                  message.size(), mac.data(), mac.size(), &mac_size) == nullptr ||
        mac_size != mac.size()) {
        throw std::runtime_error("cipher: HMAC-SHA256 failed");
    }
    Key16 iv{};
    for (std::size_t i = 0; i < iv.size(); ++i) iv[i] = mac[i];
    return iv;
}

std::vector<std::uint8_t> encrypt(const RpCipher& cipher, std::uint64_t counter, std::span<const std::uint8_t> plain) {
    return run_cipher(cipher, counter, plain, true);
}

std::vector<std::uint8_t> decrypt(const RpCipher& cipher, std::uint64_t counter, std::span<const std::uint8_t> sealed) {
    return run_cipher(cipher, counter, sealed, false);
}

Key16 random_key() {
    Key16 key{};
    if (RAND_bytes(key.data(), static_cast<int>(key.size())) != 1) {
        throw std::runtime_error("cipher: the system's random generator failed");
    }
    return key;
}

}  // namespace psvp::proto
