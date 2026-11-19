#include "app/account_id.h"

#include <algorithm>
#include <charconv>
#include <cstdint>

#include "proto/base64.h"

namespace psvp::app {

std::optional<proto::AccountId> parse_account_id(std::string_view text) {
    proto::AccountId id{};
    // Base64 of eight bytes is always twelve characters ending in one '='.
    if (text.size() == 12 && text.back() == '=') {
        const auto bytes = proto::base64_decode(text);
        if (!bytes || bytes->size() != id.size()) return std::nullopt;
        std::copy(bytes->begin(), bytes->end(), id.begin());
        return id;
    }
    std::uint64_t number = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number, 10);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    for (std::size_t i = 0; i < id.size(); ++i) id[i] = static_cast<std::uint8_t>(number >> (8 * i));
    return id;
}

std::string account_id_base64(const proto::AccountId& id) { return proto::base64_encode(id); }

std::string account_id_decimal(const proto::AccountId& id) {
    std::uint64_t number = 0;
    for (std::size_t i = 0; i < id.size(); ++i) number |= std::uint64_t{id[i]} << (8 * i);
    return std::to_string(number);
}

}  // namespace psvp::app
