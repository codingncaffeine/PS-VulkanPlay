// Fuzz target for the base64 codec. Two properties must hold for any input:
// bytes survive encoding and decoding, and text is either refused or is exactly
// the encoding of the bytes it decodes to.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>

#include "proto/base64.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using psvp::proto::base64_decode;
    using psvp::proto::base64_encode;

    const std::span<const std::uint8_t> bytes(data, size);
    const std::string text = base64_encode(bytes);
    const auto back = base64_decode(text);
    if (!back || !std::ranges::equal(*back, bytes)) std::abort();

    const std::string_view raw(reinterpret_cast<const char*>(data), size);
    if (const auto decoded = base64_decode(raw)) {
        if (base64_encode(*decoded) != raw) std::abort();
    }
    return 0;
}
