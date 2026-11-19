// Fuzz target for the text-message reader and the discovery reply built on it.
// Whatever arrives in a datagram must be read or refused, never crash, and
// never yield more headers than the limit.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "proto/discovery.h"
#include "proto/text_message.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    if (const auto block = psvp::proto::parse_header_block(text)) {
        if (block->headers.size() > psvp::proto::kMaxTextHeaders) std::abort();
        for (const auto& header : block->headers) {
            if (header.name.empty()) std::abort();
        }
    }
    if (const auto message = psvp::proto::parse_status_message(text)) {
        if (message->status <= 0 || message->status > 999) std::abort();
    }
    if (const auto reply = psvp::proto::parse_discovery_reply(text)) {
        (void)reply->supported();
    }
    return 0;
}
