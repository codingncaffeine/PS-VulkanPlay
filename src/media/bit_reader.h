// Reading the units of an H.264 or HEVC byte stream and the bits inside them
// (ITU-T H.264 7.2 and annex B, ITU-T H.265 7.2 and annex B). No I/O.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace psvp::media::detail {

// One NAL unit of a byte stream: its header and payload, between the start codes.
struct Unit {
    std::size_t begin = 0;
    std::size_t end = 0;
};

inline std::vector<Unit> split_units(std::span<const std::uint8_t> bytes) {
    std::vector<Unit> units;
    const auto next_start = [&](std::size_t from) {
        for (std::size_t i = from; i + 2 < bytes.size(); ++i) {
            if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1) return i;
        }
        return bytes.size();
    };
    for (std::size_t start = next_start(0); start < bytes.size();) {
        const std::size_t begin = start + 3;
        const std::size_t following = next_start(begin);
        // Zero bytes before the next start code, or at the end, are filling: a unit never ends in one.
        std::size_t end = following;
        while (end > begin && bytes[end - 1] == 0) --end;
        units.push_back(Unit{begin, end});
        start = following;
    }
    return units;
}

// A unit's bytes without the bytes that keep start codes from showing up inside it (a 3 after two zeros).
inline std::vector<std::uint8_t> unescape(std::span<const std::uint8_t> unit) {
    std::vector<std::uint8_t> out;
    out.reserve(unit.size());
    unsigned zeros = 0;
    for (const std::uint8_t byte : unit) {
        if (zeros >= 2 && byte == 3) {
            zeros = 0;
            continue;
        }
        out.push_back(byte);
        zeros = byte == 0 ? zeros + 1 : 0;
    }
    return out;
}

// Reads bits, the first bit of a byte being its highest. Past the end it reads zeros and remembers.
class Bits {
public:
    explicit Bits(std::span<const std::uint8_t> data) : data_(data) {}
    std::uint32_t u(unsigned count) {
        std::uint32_t value = 0;
        for (unsigned i = 0; i < count; ++i) {
            if (at_ >= data_.size() * 8) {
                failed_ = true;
                return 0;
            }
            value = (value << 1) | ((static_cast<std::uint32_t>(data_[at_ >> 3]) >> (7 - (at_ & 7))) & 1U);
            ++at_;
        }
        return value;
    }
    bool flag() { return u(1) != 0; }
    void skip(std::size_t count) {
        if (count > data_.size() * 8 - at_) {
            failed_ = true;
            at_ = data_.size() * 8;
        } else {
            at_ += count;
        }
    }
    // An unsigned number of the variable-length kind, ue(v): so many zeros, a one, as many more bits.
    std::uint32_t ue() {
        unsigned zeros = 0;
        while (!failed_ && u(1) == 0) {
            if (++zeros > 31) failed_ = true;
        }
        if (failed_) return 0;
        return ((std::uint32_t{1} << zeros) - 1U) + u(zeros);
    }
    void se() { (void)ue(); }  // a signed one takes the same bits
    // A signed one's value, se(v): 1, -1, 2, -2, ... for 1, 2, 3, 4, ...
    std::int32_t se_value() {
        const std::uint32_t coded = ue();
        return (coded & 1U) != 0 ? static_cast<std::int32_t>((coded + 1) / 2) : -static_cast<std::int32_t>(coded / 2);
    }
    [[nodiscard]] std::size_t position() const noexcept { return at_; }
    [[nodiscard]] bool failed() const noexcept { return failed_; }

private:
    std::span<const std::uint8_t> data_;
    std::size_t at_ = 0;
    bool failed_ = false;
};

}  // namespace psvp::media::detail
