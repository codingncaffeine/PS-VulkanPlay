#include "proto/input.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "proto/transport.h"

namespace psvp::proto::transport {
namespace {

constexpr float kGyroRange = 30.0F;
constexpr float kAccelRange = 5.0F;

// A sensor value in [-range, range] as a number in [0, 65535], rounded down.
std::uint16_t scaled(float value, float range) {
    const float held = std::clamp(value, -range, range);
    const float number = 65535.0F * (held - -range) / (range - -range);
    return static_cast<std::uint16_t>(number);
}

// A rotation in 32 bits: which of its four parts is the largest and that part's sign, then the
// other three in nine bits each. The largest follows from the others, the rotation's length being 1.
std::uint32_t packed_rotation(const float q[4]) {
    std::size_t largest = 0;
    for (std::size_t i = 1; i < 4; ++i) {
        if (std::fabs(q[i]) > std::fabs(q[largest])) largest = i;
    }
    constexpr double kLimit = std::numbers::sqrt2 / 2.0;  // no other part can be larger than this
    std::uint32_t packed = (q[largest] < 0.0F ? 1U : 0U) | static_cast<std::uint32_t>(largest << 1);
    for (std::size_t i = 0; i < 3; ++i) {
        float part = q[i < largest ? i : i + 1];
        if (static_cast<double>(part) < -kLimit) part = static_cast<float>(-kLimit);
        if (static_cast<double>(part) > kLimit) part = static_cast<float>(kLimit);
        part = static_cast<float>(static_cast<double>(part) + kLimit);
        part = static_cast<float>(static_cast<double>(part) * (511.0 / (2.0 * kLimit)));
        packed |= static_cast<std::uint32_t>(part) << (3 + i * 9);
    }
    return packed;
}

}  // namespace

std::vector<std::uint8_t> encode_input_state(ConsoleKind kind, const InputState& state) {
    std::vector<std::uint8_t> out(kind == ConsoleKind::ps5 ? kInputStateSizePs5 : kInputStateSizePs4, 0);
    out[0] = 0xa0;
    const auto put_low_first = [&](std::size_t at, std::uint16_t value) {
        out[at] = static_cast<std::uint8_t>(value);
        out[at + 1] = static_cast<std::uint8_t>(value >> 8);
    };
    const auto put_high_first = [&](std::size_t at, std::int16_t value) {
        out[at] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(value) >> 8);
        out[at + 1] = static_cast<std::uint8_t>(value);
    };
    put_low_first(1, scaled(state.gyro_x, kGyroRange));
    put_low_first(3, scaled(state.gyro_y, kGyroRange));
    put_low_first(5, scaled(state.gyro_z, kGyroRange));
    put_low_first(7, scaled(state.accel_x, kAccelRange));
    put_low_first(9, scaled(state.accel_y, kAccelRange));
    put_low_first(11, scaled(state.accel_z, kAccelRange));
    const float rotation[4] = {state.orient_x, state.orient_y, state.orient_z, state.orient_w};
    const std::uint32_t packed = packed_rotation(rotation);
    put_low_first(13, static_cast<std::uint16_t>(packed));
    put_low_first(15, static_cast<std::uint16_t>(packed >> 16));
    put_high_first(17, state.left_x);
    put_high_first(19, state.left_y);
    put_high_first(21, state.right_x);
    put_high_first(23, state.right_y);
    if (kind == ConsoleKind::ps5) out[27] = 0x01;
    return out;
}

std::optional<InputStateFields> parse_input_state(ConsoleKind kind, std::span<const std::uint8_t> bytes) {
    const bool ps5 = kind == ConsoleKind::ps5;
    if (bytes.size() != (ps5 ? kInputStateSizePs5 : kInputStateSizePs4) || bytes[0] != 0xa0) return std::nullopt;
    if (ps5 && (bytes[25] != 0 || bytes[26] != 0 || bytes[27] != 1)) return std::nullopt;
    const auto low_first = [&](std::size_t at) { return static_cast<std::uint16_t>(bytes[at] | (bytes[at + 1] << 8)); };
    const auto high_first = [&](std::size_t at) { return static_cast<std::int16_t>(static_cast<std::uint16_t>((bytes[at] << 8) | bytes[at + 1])); };
    InputStateFields fields;
    for (std::size_t i = 0; i < 3; ++i) {
        fields.gyro[i] = low_first(1 + 2 * i);
        fields.accel[i] = low_first(7 + 2 * i);
    }
    fields.orientation = std::uint32_t{low_first(13)} | std::uint32_t{low_first(15)} << 16;
    fields.left_x = high_first(17);
    fields.left_y = high_first(19);
    fields.right_x = high_first(21);
    fields.right_y = high_first(23);
    return fields;
}

std::vector<std::uint8_t> encode_input_packet(std::uint16_t sequence, std::span<const std::uint8_t> state) {
    std::vector<std::uint8_t> out(kInputHeaderSize, 0);
    out[0] = packet_kind::kInputState;
    out[1] = static_cast<std::uint8_t>(sequence >> 8);
    out[2] = static_cast<std::uint8_t>(sequence);
    // The fourth byte: how many states the packet carries, less one.
    out.insert(out.end(), state.begin(), state.end());
    return out;
}

}  // namespace psvp::proto::transport
