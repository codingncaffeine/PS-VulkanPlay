// Erasure repair: a frame travels as k source units and m repair units, and
// any k of them give back the frame. docs/protocol/video.md describes the code
// (a Cauchy matrix over the field of 256 elements). No sockets, no clock.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace psvp::proto {

// The field of 256 elements with the polynomial x^8 + x^4 + x^3 + x^2 + 1.
[[nodiscard]] std::uint8_t gf256_multiply(std::uint8_t a, std::uint8_t b) noexcept;
// 1 / a; a is not zero.
[[nodiscard]] std::uint8_t gf256_inverse(std::uint8_t a) noexcept;

// The factor of source unit `source` in repair unit `repair`, of `repair_units` in all.
[[nodiscard]] std::uint8_t repair_factor(unsigned repair, unsigned source, unsigned repair_units) noexcept;

// Computes the repair units of a frame (what a console does), for tests and the mock console.
// `frame` holds source_units + repair_units units of `unit_size` bytes, `stride` bytes apart;
// the repair units are written behind the source units. False for sizes that do not add up.
bool make_repair_units(std::span<std::uint8_t> frame, std::size_t unit_size, std::size_t stride, unsigned source_units,
                       unsigned repair_units);

// Restores the source units that are missing, in place. `missing` lists the units that did not
// arrive, source and repair alike, in rising order. False when there are more than repair_units of
// them, or the sizes do not add up; the frame is then left as it was.
bool repair_units(std::span<std::uint8_t> frame, std::size_t unit_size, std::size_t stride, unsigned source_units,
                  unsigned repair_units, std::span<const unsigned> missing);

}  // namespace psvp::proto
