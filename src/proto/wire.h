// The field coding the stream's messages are written in (the "protocol
// buffers" wire format): numbered fields, each a variable-length number, a
// fixed 32- or 64-bit value, or a length followed by bytes.
// docs/protocol/messages.md describes it.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace psvp::proto::wire {

enum class Kind : std::uint8_t { varint = 0, fixed64 = 1, bytes = 2, fixed32 = 5 };

class Writer {
public:
    void varint(std::uint32_t field, std::uint64_t value);
    void boolean(std::uint32_t field, bool value) { varint(field, value ? 1 : 0); }
    void bytes(std::uint32_t field, std::span<const std::uint8_t> value);
    void text(std::uint32_t field, std::string_view value);
    // A message inside this one.
    void message(std::uint32_t field, const Writer& inner) { bytes(field, inner.out_); }

    [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept { return out_; }
    [[nodiscard]] std::vector<std::uint8_t> take() noexcept { return std::move(out_); }

private:
    void raw_varint(std::uint64_t value);
    std::vector<std::uint8_t> out_;
};

struct Field {
    std::uint32_t number = 0;
    Kind kind = Kind::varint;
    std::uint64_t value = 0;               // varint, fixed32 and fixed64
    std::span<const std::uint8_t> bytes;   // Kind::bytes; a view into what the reader was given
};

// Walks the fields of one message. After the last field next() gives nothing
// and ok() tells whether the message was read to its end without a fault.
class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> message) : rest_(message) {}

    [[nodiscard]] std::optional<Field> next();
    [[nodiscard]] bool ok() const noexcept { return !failed_ && rest_.empty(); }

private:
    std::optional<std::uint64_t> raw_varint();
    std::optional<Field> fail();

    std::span<const std::uint8_t> rest_;
    bool failed_ = false;
};

}  // namespace psvp::proto::wire
