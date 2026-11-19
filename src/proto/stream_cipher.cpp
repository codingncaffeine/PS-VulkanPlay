#include "proto/stream_cipher.h"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <vector>

#include <openssl/evp.h>

namespace psvp::proto {
namespace {

constexpr std::size_t kBlock = 16;
// How far the counter moves for each change of the authentication key.
constexpr std::uint64_t kMacKeyCounterStep = 44910;

struct ContextFree {
    void operator()(EVP_CIPHER_CTX* context) const noexcept { EVP_CIPHER_CTX_free(context); }
};
using ContextPtr = std::unique_ptr<EVP_CIPHER_CTX, ContextFree>;

// `base` taken as a number with its least significant byte first, plus `value`.
Key16 counter_plus(const Key16& base, std::uint64_t value) {
    Key16 out = base;
    for (std::size_t i = 0; i < out.size() && value != 0; ++i) {
        const std::uint64_t sum = std::uint64_t{base[i]} + value;
        out[i] = static_cast<std::uint8_t>(sum);
        value = sum >> 8;
    }
    return out;
}

// SHA-256 of `key` followed by `counter`, its two halves combined by exclusive or.
Key16 folded_hash(const Key16& key, const Key16& counter) {
    std::array<std::uint8_t, 32> message{};
    std::copy(key.begin(), key.end(), message.begin());
    std::copy(counter.begin(), counter.end(), message.begin() + 16);
    std::array<std::uint8_t, 32> digest{};
    std::size_t size = 0;
    if (EVP_Q_digest(nullptr, "SHA256", nullptr, message.data(), message.size(), digest.data(), &size) != 1 || size != digest.size()) {
        throw std::runtime_error("stream cipher: SHA-256 failed");
    }
    Key16 out{};
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = static_cast<std::uint8_t>(digest[i] ^ digest[i + 16]);
    return out;
}

}  // namespace

StreamCipher::StreamCipher(std::uint8_t direction, const Key16& handshake_key, const SharedSecret& secret) {
    // HMAC-SHA256 under the shared secret of: 01, the direction, 00, the handshake key, 01, 00.
    std::array<std::uint8_t, 3 + 16 + 2> message{};
    message[0] = 1;
    message[1] = direction;
    std::copy(handshake_key.begin(), handshake_key.end(), message.begin() + 3);
    message[19] = 1;
    std::array<std::uint8_t, 32> mac{};
    std::size_t size = 0;
    if (EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA256", nullptr, secret.data(), secret.size(), message.data(), message.size(), mac.data(),
                  mac.size(), &size) == nullptr ||
        size != mac.size()) {
        throw std::runtime_error("stream cipher: HMAC-SHA256 failed");
    }
    std::copy_n(mac.begin(), 16, key_.begin());
    std::copy_n(mac.begin() + 16, 16, iv_.begin());
    mac_base_ = folded_hash(key_, iv_);
}

Key16 StreamCipher::mac_key(std::uint64_t key_position) const {
    const std::uint64_t index = (key_position > 0 ? key_position - 1 : 0) / kMacKeySpan;
    if (index == 0) return mac_base_;
    return folded_hash(mac_base_, counter_plus(iv_, index * kMacKeyCounterStep));
}

Mac4 StreamCipher::mac(std::uint64_t key_position, std::initializer_list<std::span<const std::uint8_t>> parts) const {
    const Key16 key = mac_key(key_position);
    const Key16 iv = counter_plus(iv_, key_position / kBlock);
    const ContextPtr context(EVP_CIPHER_CTX_new());
    // AES-128-GCM with a sixteen-byte starting vector, nothing encrypted, everything authenticated.
    if (!context || EVP_EncryptInit_ex(context.get(), EVP_aes_128_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv.size()), nullptr) != 1 ||
        EVP_EncryptInit_ex(context.get(), nullptr, nullptr, key.data(), iv.data()) != 1) {
        throw std::runtime_error("stream cipher: cannot start AES-128-GCM");
    }
    int written = 0;
    for (const std::span<const std::uint8_t> part : parts) {
        if (part.empty()) continue;
        if (EVP_EncryptUpdate(context.get(), nullptr, &written, part.data(), static_cast<int>(part.size())) != 1) {
            throw std::runtime_error("stream cipher: AES-128-GCM failed");
        }
    }
    std::uint8_t none = 0;
    Mac4 code{};
    if (EVP_EncryptFinal_ex(context.get(), &none, &written) != 1 ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(code.size()), code.data()) != 1) {
        throw std::runtime_error("stream cipher: AES-128-GCM failed");
    }
    return code;
}

void StreamCipher::apply_key_stream(std::uint64_t key_position, std::span<std::uint8_t> data) const {
    if (data.empty()) return;
    // The key stream is AES-128 of a counter, one block per sixteen bytes of key position.
    const std::size_t skip = static_cast<std::size_t>(key_position % kBlock);
    const std::size_t blocks = (skip + data.size() + kBlock - 1) / kBlock;
    std::vector<std::uint8_t> stream(blocks * kBlock);
    std::uint64_t counter = key_position / kBlock;
    for (std::size_t block = 0; block < blocks; ++block) {
        const Key16 value = counter_plus(iv_, counter++);
        std::copy(value.begin(), value.end(), stream.begin() + static_cast<std::ptrdiff_t>(block * kBlock));
    }
    const ContextPtr context(EVP_CIPHER_CTX_new());
    int written = 0;
    if (!context || EVP_EncryptInit_ex(context.get(), EVP_aes_128_ecb(), nullptr, key_.data(), nullptr) != 1 ||
        EVP_CIPHER_CTX_set_padding(context.get(), 0) != 1 ||
        EVP_EncryptUpdate(context.get(), stream.data(), &written, stream.data(), static_cast<int>(stream.size())) != 1 ||
        static_cast<std::size_t>(written) != stream.size()) {
        throw std::runtime_error("stream cipher: AES-128 failed");
    }
    for (std::size_t i = 0; i < data.size(); ++i) data[i] ^= stream[skip + i];
}

std::uint64_t KeyPositionTracker::extend(std::uint32_t low) const noexcept {
    const auto last_low = static_cast<std::uint32_t>(last_);
    std::uint64_t high = last_ >> 32;
    // Ahead or behind by less than half the number space; exactly half is neither.
    const bool ahead = low != last_low && low - last_low < 0x80000000U;
    const bool behind = low != last_low && last_low - low < 0x80000000U;
    if (ahead && low < last_low) ++high;                       // went past the top
    else if (behind && low > last_low && high != 0) --high;    // a late packet from before the wrap
    return (high << 32) | low;
}

}  // namespace psvp::proto
