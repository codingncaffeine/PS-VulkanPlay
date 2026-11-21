// Writing the bits of a stream's units, as H.264 (ITU-T H.264, 7.2 and 9.1) and
// HEVC (ITU-T H.265, 7.2 and 9.2) both write them. Used by the generated clips.
#pragma once

#include <bit>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace psvp::media::detail {

// Bits, highest first.
class BitWriter {
public:
    void bits(std::uint32_t value, unsigned count) {
        for (unsigned i = count; i-- > 0;) {
            if (used_ == 0) bytes_.push_back(0);
            if (((value >> i) & 1U) != 0) bytes_.back() |= static_cast<std::uint8_t>(0x80U >> used_);
            used_ = (used_ + 1) % 8;
        }
    }
    // An unsigned number in the exponential Golomb code: ue(v).
    void ue(std::uint32_t value) {
        const std::uint32_t coded = value + 1;
        const auto width = static_cast<unsigned>(std::bit_width(coded));
        bits(0, width - 1);
        bits(coded, width);
    }
    // A signed one: se(v).
    void se(std::int32_t value) { ue(value > 0 ? static_cast<std::uint32_t>(2 * value - 1) : static_cast<std::uint32_t>(-2 * value)); }
    void align() { used_ = 0; }  // the bits left of the last byte stay zero
    [[nodiscard]] bool aligned() const noexcept { return used_ == 0; }
    void byte(std::uint8_t value) { bytes_.push_back(value); }  // only when aligned
    // rbsp_trailing_bits: a one, then zeros to the end of the byte.
    void finish() {
        bits(1, 1);
        align();
    }
    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }

private:
    std::vector<std::uint8_t> bytes_;
    unsigned used_ = 0;  // bits used of the last byte; 0: none begun
};

// A unit behind a four-byte start code: its header bytes (one for H.264, two for HEVC), then
// the payload, in which two zero bytes are never followed by a byte below four: a 3 is put in
// between (H.264 7.4.1, H.265 7.4.2).
inline void append_unit(std::vector<std::uint8_t>& out, std::initializer_list<std::uint8_t> header, const std::vector<std::uint8_t>& payload) {
    out.insert(out.end(), {0, 0, 0, 1});
    out.insert(out.end(), header);
    unsigned zeros = 0;
    for (const std::uint8_t byte : payload) {
        if (zeros >= 2 && byte <= 3) {
            out.push_back(3);
            zeros = 0;
        }
        out.push_back(byte);
        zeros = byte == 0 ? zeros + 1 : 0;
    }
}

}  // namespace psvp::media::detail
