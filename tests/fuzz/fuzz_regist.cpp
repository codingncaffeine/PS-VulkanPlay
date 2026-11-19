// Fuzz target for the registration response: the head as received, and a body
// that decrypts to arbitrary bytes (which is what a wrong PIN or a hostile
// answer gives).
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string_view>

#include "proto/regist.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace psvp::proto;
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    if (const auto length = parse_regist_response_head(text)) {
        if (*length == 0 || *length > kMaxRegistBody) std::abort();
    } else {
        (void)describe(length.error());
    }

    // The same bytes as an encrypted body: sealed here so that the reader sees
    // them as its plain text, under both families' field names.
    static const Key16 nonce{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    for (const ConsoleKind kind : {ConsoleKind::ps4, ConsoleKind::ps5}) {
        const RpCipher cipher = make_regist_cipher(kind, nonce, 1, 12345678).value();
        const auto sealed = encrypt(cipher, 0, std::span(data, size));
        if (const auto console = parse_regist_response_body(cipher, sealed)) {
            if (console->regist_key.empty() || console->regist_key.size() > 16) std::abort();
        }
    }
    return 0;
}
