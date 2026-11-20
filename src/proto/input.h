// What the client tells the console about its controller: the input state
// packet with the sticks and the motion sensors. docs/protocol/input.md
// describes it. Buttons travel apart from it and come with the controllers.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "proto/console.h"

namespace psvp::proto::transport {

// A controller at rest on a table: nothing moves, gravity pulls along y.
struct InputState {
    float gyro_x = 0.0F, gyro_y = 0.0F, gyro_z = 0.0F;           // -30 to 30
    float accel_x = 0.0F, accel_y = 1.0F, accel_z = 0.0F;        // -5 to 5
    float orient_x = 0.0F, orient_y = 0.0F, orient_z = 0.0F, orient_w = 1.0F;  // a rotation, of length 1
    std::int16_t left_x = 0, left_y = 0, right_x = 0, right_y = 0;
    friend bool operator==(const InputState&, const InputState&) = default;
};

inline constexpr std::size_t kInputHeaderSize = 12;
inline constexpr std::size_t kInputStateSizePs4 = 25;
inline constexpr std::size_t kInputStateSizePs5 = 28;

// The state as it travels: 25 bytes to a PS4, 28 to a PS5. Sensor values outside their range are held at its ends.
[[nodiscard]] std::vector<std::uint8_t> encode_input_state(ConsoleKind kind, const InputState& state);

// What can be read back from those bytes, for the mock console and the tests: the sticks, and
// the sensors as the 16-bit numbers they travel as.
struct InputStateFields {
    std::uint16_t gyro[3] = {};
    std::uint16_t accel[3] = {};
    std::uint32_t orientation = 0;
    std::int16_t left_x = 0, left_y = 0, right_x = 0, right_y = 0;
};
[[nodiscard]] std::optional<InputStateFields> parse_input_state(ConsoleKind kind, std::span<const std::uint8_t> bytes);

// An input state packet around a state that is already encrypted: its header with the code
// and the key position left zero (seal_datagram writes them), then the state.
[[nodiscard]] std::vector<std::uint8_t> encode_input_packet(std::uint16_t sequence, std::span<const std::uint8_t> state);

}  // namespace psvp::proto::transport
