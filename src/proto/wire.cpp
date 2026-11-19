#include "proto/wire.h"

namespace psvp::proto::wire {
namespace {

constexpr std::uint32_t kMaxFieldNumber = (1U << 29) - 1;

}  // namespace

void Writer::raw_varint(std::uint64_t value) {
    while (value >= 0x80) {
        out_.push_back(static_cast<std::uint8_t>(value | 0x80));
        value >>= 7;
    }
    out_.push_back(static_cast<std::uint8_t>(value));
}

void Writer::varint(std::uint32_t field, std::uint64_t value) {
    raw_varint((std::uint64_t{field} << 3) | static_cast<std::uint64_t>(Kind::varint));
    raw_varint(value);
}

void Writer::bytes(std::uint32_t field, std::span<const std::uint8_t> value) {
    raw_varint((std::uint64_t{field} << 3) | static_cast<std::uint64_t>(Kind::bytes));
    raw_varint(value.size());
    out_.insert(out_.end(), value.begin(), value.end());
}

void Writer::text(std::uint32_t field, std::string_view value) {
    bytes(field, {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

std::optional<Field> Reader::fail() {
    failed_ = true;
    rest_ = {};
    return std::nullopt;
}

std::optional<std::uint64_t> Reader::raw_varint() {
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        if (rest_.empty()) return std::nullopt;
        const std::uint8_t byte = rest_.front();
        rest_ = rest_.subspan(1);
        // The tenth byte holds one bit only.
        if (shift == 63 && byte > 1) return std::nullopt;
        value |= std::uint64_t{byte & 0x7fU} << shift;
        if ((byte & 0x80U) == 0) return value;
    }
    return std::nullopt;
}

std::optional<Field> Reader::next() {
    if (failed_ || rest_.empty()) return std::nullopt;
    const auto key = raw_varint();
    if (!key || (*key >> 3) == 0 || (*key >> 3) > kMaxFieldNumber) return fail();

    Field field;
    field.number = static_cast<std::uint32_t>(*key >> 3);
    switch (*key & 7U) {
    case 0: {
        const auto value = raw_varint();
        if (!value) return fail();
        field.kind = Kind::varint;
        field.value = *value;
        return field;
    }
    case 1:
    case 5: {
        const std::size_t size = (*key & 7U) == 1 ? 8 : 4;
        if (rest_.size() < size) return fail();
        field.kind = size == 8 ? Kind::fixed64 : Kind::fixed32;
        for (std::size_t i = 0; i < size; ++i) field.value |= std::uint64_t{rest_[i]} << (8 * i);
        rest_ = rest_.subspan(size);
        return field;
    }
    case 2: {
        const auto size = raw_varint();
        if (!size || *size > rest_.size()) return fail();
        field.kind = Kind::bytes;
        field.bytes = rest_.first(static_cast<std::size_t>(*size));
        rest_ = rest_.subspan(static_cast<std::size_t>(*size));
        return field;
    }
    default:
        // Groups (3 and 4) are not used by the protocol; 6 and 7 do not exist.
        return fail();
    }
}

}  // namespace psvp::proto::wire
