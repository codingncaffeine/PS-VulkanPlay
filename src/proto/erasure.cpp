#include "proto/erasure.h"

#include <algorithm>
#include <array>
#include <vector>

namespace psvp::proto {
namespace {

struct Tables {
    std::array<std::uint8_t, 512> power{};  // power[i] = 2^i, twice over so that sums of two logarithms need no wrap
    std::array<std::uint8_t, 256> log{};
};

constexpr Tables make_tables() {
    Tables t;
    unsigned value = 1;
    for (unsigned i = 0; i < 255; ++i) {
        t.power[i] = static_cast<std::uint8_t>(value);
        t.power[i + 255] = static_cast<std::uint8_t>(value);
        t.log[value] = static_cast<std::uint8_t>(i);
        value <<= 1;
        if ((value & 0x100U) != 0) value ^= 0x11dU;
    }
    return t;
}
constexpr Tables kTables = make_tables();

// out ^= factor * in, byte by byte.
void add_scaled(std::span<std::uint8_t> out, std::span<const std::uint8_t> in, std::uint8_t factor) {
    if (factor == 0) return;
    if (factor == 1) {
        for (std::size_t i = 0; i < out.size(); ++i) out[i] ^= in[i];
        return;
    }
    std::array<std::uint8_t, 256> row{};
    const unsigned log_factor = kTables.log[factor];
    for (unsigned v = 1; v < 256; ++v) row[v] = kTables.power[log_factor + kTables.log[v]];
    for (std::size_t i = 0; i < out.size(); ++i) out[i] ^= row[in[i]];
}

void scale(std::span<std::uint8_t> data, std::uint8_t factor) {
    if (factor == 1) return;
    std::array<std::uint8_t, 256> row{};
    const unsigned log_factor = kTables.log[factor];
    for (unsigned v = 1; v < 256; ++v) row[v] = kTables.power[log_factor + kTables.log[v]];
    for (std::uint8_t& byte : data) byte = row[byte];
}

bool sizes_fit(std::size_t frame_size, std::size_t unit_size, std::size_t stride, unsigned source_units, unsigned repair_units) {
    const std::size_t units = std::size_t{source_units} + repair_units;
    return unit_size != 0 && stride >= unit_size && source_units != 0 && units <= 256 && frame_size >= (units - 1) * stride + unit_size;
}

}  // namespace

std::uint8_t gf256_multiply(std::uint8_t a, std::uint8_t b) noexcept {
    if (a == 0 || b == 0) return 0;
    return kTables.power[static_cast<unsigned>(kTables.log[a]) + kTables.log[b]];
}

std::uint8_t gf256_inverse(std::uint8_t a) noexcept { return kTables.power[255U - kTables.log[a]]; }

std::uint8_t repair_factor(unsigned repair, unsigned source, unsigned repair_units) noexcept {
    return gf256_inverse(static_cast<std::uint8_t>(repair ^ (repair_units + source)));
}

bool make_repair_units(std::span<std::uint8_t> frame, std::size_t unit_size, std::size_t stride, unsigned source_units,
                       unsigned repair_units) {
    if (!sizes_fit(frame.size(), unit_size, stride, source_units, repair_units)) return false;
    for (unsigned r = 0; r < repair_units; ++r) {
        const std::span<std::uint8_t> out = frame.subspan((std::size_t{source_units} + r) * stride, unit_size);
        std::fill(out.begin(), out.end(), std::uint8_t{0});
        for (unsigned s = 0; s < source_units; ++s) add_scaled(out, frame.subspan(std::size_t{s} * stride, unit_size), repair_factor(r, s, repair_units));
    }
    return true;
}

bool repair_units(std::span<std::uint8_t> frame, std::size_t unit_size, std::size_t stride, unsigned source_units,
                  unsigned repair_units, std::span<const unsigned> missing) {
    if (!sizes_fit(frame.size(), unit_size, stride, source_units, repair_units)) return false;
    if (missing.size() > repair_units) return false;
    const unsigned units = source_units + repair_units;
    std::vector<bool> gone(units, false);
    for (const unsigned index : missing) {
        if (index >= units || gone[index]) return false;
        gone[index] = true;
    }
    std::vector<unsigned> lost;  // the source units to restore
    for (unsigned s = 0; s < source_units; ++s) {
        if (gone[s]) lost.push_back(s);
    }
    if (lost.empty()) return true;
    std::vector<unsigned> helpers;  // as many repair units that arrived
    for (unsigned r = 0; r < repair_units && helpers.size() < lost.size(); ++r) {
        if (!gone[source_units + r]) helpers.push_back(r);
    }
    if (helpers.size() < lost.size()) return false;

    // Each helper is a sum over all source units. Taking out the ones that arrived leaves
    // n equations in the n lost units; they are solved in a work area, then copied in.
    const std::size_t n = lost.size();
    std::vector<std::uint8_t> work(n * unit_size);
    std::vector<std::uint8_t> matrix(n * n);
    for (std::size_t row = 0; row < n; ++row) {
        const std::span<std::uint8_t> sum(work.data() + row * unit_size, unit_size);
        const auto helper = frame.subspan((std::size_t{source_units} + helpers[row]) * stride, unit_size);
        std::copy(helper.begin(), helper.end(), sum.begin());
        for (unsigned s = 0; s < source_units; ++s) {
            if (!gone[s]) add_scaled(sum, frame.subspan(std::size_t{s} * stride, unit_size), repair_factor(helpers[row], s, repair_units));
        }
        for (std::size_t column = 0; column < n; ++column) matrix[row * n + column] = repair_factor(helpers[row], lost[column], repair_units);
    }
    // Elimination. Every square part of a Cauchy matrix can be inverted, so a pivot is always found.
    for (std::size_t column = 0; column < n; ++column) {
        std::size_t pivot = column;
        while (pivot < n && matrix[pivot * n + column] == 0) ++pivot;
        if (pivot == n) return false;
        if (pivot != column) {
            for (std::size_t c = 0; c < n; ++c) std::swap(matrix[pivot * n + c], matrix[column * n + c]);
            std::swap_ranges(work.begin() + static_cast<std::ptrdiff_t>(pivot * unit_size),
                             work.begin() + static_cast<std::ptrdiff_t>((pivot + 1) * unit_size),
                             work.begin() + static_cast<std::ptrdiff_t>(column * unit_size));
        }
        const std::uint8_t inverse = gf256_inverse(matrix[column * n + column]);
        for (std::size_t c = 0; c < n; ++c) matrix[column * n + c] = gf256_multiply(matrix[column * n + c], inverse);
        scale(std::span<std::uint8_t>(work.data() + column * unit_size, unit_size), inverse);
        for (std::size_t row = 0; row < n; ++row) {
            const std::uint8_t factor = matrix[row * n + column];
            if (row == column || factor == 0) continue;
            for (std::size_t c = 0; c < n; ++c) matrix[row * n + c] ^= gf256_multiply(matrix[column * n + c], factor);
            add_scaled(std::span<std::uint8_t>(work.data() + row * unit_size, unit_size),
                       std::span<const std::uint8_t>(work.data() + column * unit_size, unit_size), factor);
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        std::copy_n(work.begin() + static_cast<std::ptrdiff_t>(i * unit_size), unit_size,
                    frame.begin() + static_cast<std::ptrdiff_t>(std::size_t{lost[i]} * stride));
    }
    return true;
}

}  // namespace psvp::proto
