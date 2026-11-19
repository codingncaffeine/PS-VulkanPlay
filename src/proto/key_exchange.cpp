#include "proto/key_exchange.h"

#include <stdexcept>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/param_build.h>
#include <openssl/params.h>

namespace psvp::proto {
namespace {

constexpr char kCurve[] = "secp256k1";

template <typename T, void (*Free)(T*)> struct Freer {
    void operator()(T* object) const noexcept { Free(object); }
};
using PkeyPtr = std::unique_ptr<EVP_PKEY, Freer<EVP_PKEY, EVP_PKEY_free>>;
using ContextPtr = std::unique_ptr<EVP_PKEY_CTX, Freer<EVP_PKEY_CTX, EVP_PKEY_CTX_free>>;
using BuilderPtr = std::unique_ptr<OSSL_PARAM_BLD, Freer<OSSL_PARAM_BLD, OSSL_PARAM_BLD_free>>;
using ParamsPtr = std::unique_ptr<OSSL_PARAM, Freer<OSSL_PARAM, OSSL_PARAM_free>>;
using NumberPtr = std::unique_ptr<BIGNUM, Freer<BIGNUM, BN_clear_free>>;
using GroupPtr = std::unique_ptr<EC_GROUP, Freer<EC_GROUP, EC_GROUP_free>>;
using PointPtr = std::unique_ptr<EC_POINT, Freer<EC_POINT, EC_POINT_free>>;

void free_key(void* key) noexcept { EVP_PKEY_free(static_cast<EVP_PKEY*>(key)); }

// A key object from the curve's name, a public point and, for a whole pair, the private number.
PkeyPtr key_from(std::span<const std::uint8_t> public_point, const BIGNUM* private_number) {
    const BuilderPtr builder(OSSL_PARAM_BLD_new());
    if (!builder || OSSL_PARAM_BLD_push_utf8_string(builder.get(), OSSL_PKEY_PARAM_GROUP_NAME, kCurve, 0) != 1 ||
        OSSL_PARAM_BLD_push_octet_string(builder.get(), OSSL_PKEY_PARAM_PUB_KEY, public_point.data(), public_point.size()) != 1) {
        return nullptr;
    }
    if (private_number != nullptr && OSSL_PARAM_BLD_push_BN(builder.get(), OSSL_PKEY_PARAM_PRIV_KEY, private_number) != 1) return nullptr;
    const ParamsPtr params(OSSL_PARAM_BLD_to_param(builder.get()));
    const ContextPtr context(EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr));
    if (!params || !context || EVP_PKEY_fromdata_init(context.get()) != 1) return nullptr;
    EVP_PKEY* key = nullptr;
    if (EVP_PKEY_fromdata(context.get(), &key, private_number != nullptr ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY, params.get()) != 1) {
        return nullptr;
    }
    return PkeyPtr(key);
}

bool read_public(EVP_PKEY* key, PublicKey& out) {
    std::size_t size = 0;
    return EVP_PKEY_get_octet_string_param(key, OSSL_PKEY_PARAM_PUB_KEY, out.data(), out.size(), &size) == 1 && size == out.size() &&
           out[0] == 0x04;
}

}  // namespace

KeyExchange KeyExchange::generate() {
    PkeyPtr key(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", kCurve));
    KeyExchange exchange;
    if (!key || !read_public(key.get(), exchange.public_)) throw std::runtime_error("key exchange: cannot make a secp256k1 key pair");
    exchange.key_ = std::shared_ptr<void>(key.release(), free_key);
    return exchange;
}

std::optional<KeyExchange> KeyExchange::from_private_key(std::span<const std::uint8_t, kPrivateKeySize> key) {
    const NumberPtr number(BN_bin2bn(key.data(), static_cast<int>(key.size()), nullptr));
    const GroupPtr group(EC_GROUP_new_by_curve_name(NID_secp256k1));
    if (!number || !group) return std::nullopt;
    // The private number is 1 or more and below the order of the curve.
    if (BN_is_zero(number.get()) || BN_cmp(number.get(), EC_GROUP_get0_order(group.get())) >= 0) return std::nullopt;
    const PointPtr point(EC_POINT_new(group.get()));
    PublicKey public_point{};
    if (!point || EC_POINT_mul(group.get(), point.get(), number.get(), nullptr, nullptr, nullptr) != 1 ||
        EC_POINT_point2oct(group.get(), point.get(), POINT_CONVERSION_UNCOMPRESSED, public_point.data(), public_point.size(), nullptr) !=
            public_point.size()) {
        return std::nullopt;
    }
    PkeyPtr pair = key_from(public_point, number.get());
    if (!pair) return std::nullopt;
    KeyExchange exchange;
    exchange.public_ = public_point;
    exchange.key_ = std::shared_ptr<void>(pair.release(), free_key);
    return exchange;
}

std::optional<SharedSecret> KeyExchange::shared_secret(std::span<const std::uint8_t> remote_public) const {
    if (remote_public.size() != kPublicKeySize || remote_public[0] != 0x04) return std::nullopt;
    const PkeyPtr remote = key_from(remote_public, nullptr);
    if (!remote) return std::nullopt;
    // A point that is not on the curve gives no secret.
    {
        const ContextPtr check(EVP_PKEY_CTX_new_from_pkey(nullptr, remote.get(), nullptr));
        if (!check || EVP_PKEY_public_check(check.get()) != 1) return std::nullopt;
    }
    const ContextPtr context(EVP_PKEY_CTX_new_from_pkey(nullptr, static_cast<EVP_PKEY*>(key_.get()), nullptr));
    SharedSecret secret{};
    std::size_t size = secret.size();
    if (!context || EVP_PKEY_derive_init(context.get()) != 1 || EVP_PKEY_derive_set_peer(context.get(), remote.get()) != 1 ||
        EVP_PKEY_derive(context.get(), secret.data(), &size) != 1 || size != secret.size()) {
        return std::nullopt;
    }
    return secret;
}

KeySignature sign_public_key(const Key16& handshake_key, std::span<const std::uint8_t> public_key) {
    KeySignature signature{};
    std::size_t size = 0;
    if (EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA256", nullptr, handshake_key.data(), handshake_key.size(), public_key.data(),
                  public_key.size(), signature.data(), signature.size(), &size) == nullptr ||
        size != signature.size()) {
        throw std::runtime_error("key exchange: HMAC-SHA256 failed");
    }
    return signature;
}

}  // namespace psvp::proto
