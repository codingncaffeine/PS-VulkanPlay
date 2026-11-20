// What the client tells the console about its controller: the input state
// packet with the sticks and the motion sensors, and the input history packet
// with the buttons, the triggers and the touchpad. docs/protocol/input.md
// describes both.
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "proto/console.h"
#include "proto/transport.h"

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

// ---- Buttons, triggers and the touchpad: the input history ----------------

enum class Button : std::uint8_t { cross, circle, square, triangle, left, right, up, down, l1, r1, l3, r3, options, share, touchpad, ps };
inline constexpr std::size_t kButtons = 16;
[[nodiscard]] std::string_view button_name(Button button) noexcept;
[[nodiscard]] std::optional<Button> button_named(std::string_view name) noexcept;

// A finger on the touchpad.
struct Touch {
    bool down = false;
    std::uint8_t id = 0;   // which touch this is, 0 to 127
    std::uint16_t x = 0;   // twelve bits each
    std::uint16_t y = 0;
    friend bool operator==(const Touch&, const Touch&) = default;
};

// Everything about a controller that travels as events.
struct Buttons {
    std::uint16_t pressed = 0;  // one bit per Button, by its number
    std::uint8_t l2 = 0;        // how far each trigger is pulled
    std::uint8_t r2 = 0;
    std::array<Touch, 2> touches{};

    [[nodiscard]] bool is_pressed(Button button) const noexcept { return (pressed >> static_cast<unsigned>(button) & 1U) != 0; }
    void set(Button button, bool down) noexcept {
        const auto bit = static_cast<std::uint16_t>(1U << static_cast<unsigned>(button));
        pressed = static_cast<std::uint16_t>(down ? pressed | bit : pressed & ~bit);
    }
    friend bool operator==(const Buttons&, const Buttons&) = default;
};

// One change as it travels: two, three or five bytes.
struct HistoryEvent {
    std::array<std::uint8_t, 5> bytes{};
    std::uint8_t size = 0;
    [[nodiscard]] std::span<const std::uint8_t> data() const noexcept { return std::span(bytes).first(size); }
};
[[nodiscard]] HistoryEvent button_event(Button button, bool pressed) noexcept;
[[nodiscard]] HistoryEvent trigger_event(bool right, std::uint8_t value) noexcept;
[[nodiscard]] HistoryEvent touch_event(const Touch& touch) noexcept;

// An event read back, for the mock console and the tests.
struct ReadEvent {
    enum class What { button, trigger, touch } what = What::button;
    Button button = Button::cross;  // What::button
    bool pressed = false;           // What::button
    bool right = false;             // What::trigger
    std::uint8_t value = 0;         // What::trigger
    Touch touch;                    // What::touch
    friend bool operator==(const ReadEvent&, const ReadEvent&) = default;
};
// The events of a history packet's payload, in its order: newest first. Nothing for bytes that are not events.
[[nodiscard]] std::optional<std::vector<ReadEvent>> parse_history_events(std::span<const std::uint8_t> payload);
[[nodiscard]] std::string describe(const ReadEvent& event);

// Keeps the newest events and says which packets are due. Every packet carries the events
// before its newest one again, so a lost packet costs nothing while the next one arrives.
class InputHistory {
public:
    static constexpr std::size_t kKept = 16;          // the most events a packet carries
    static constexpr std::size_t kKeptAfterSend = 8;  // how many ride again in the next one
    // A packet that nothing followed goes out again this long after it was first sent:
    // otherwise a lost release would leave its button held.
    static constexpr std::array<std::chrono::milliseconds, 4> kAgainAfter{std::chrono::milliseconds{20}, std::chrono::milliseconds{50},
                                                                           std::chrono::milliseconds{100}, std::chrono::milliseconds{200}};

    struct Packet {
        std::uint16_t sequence = 0;         // the number of its newest event
        std::uint8_t events = 0;
        std::vector<std::uint8_t> payload;  // the events, newest first, not encrypted
    };
    // The events between two states in the order a console expects them: releases, touches,
    // presses, triggers. One packet per event.
    [[nodiscard]] std::vector<Packet> on_change(const Buttons& before, const Buttons& now, TimePoint when);
    // The last packet again, when that is due.
    [[nodiscard]] std::optional<Packet> on_timeout(TimePoint now);
    [[nodiscard]] std::optional<TimePoint> deadline() const;

private:
    std::vector<HistoryEvent> events_;  // newest first
    std::uint16_t pushed_ = 0;          // events so far
    std::optional<Packet> last_;
    TimePoint last_sent_{};
    std::size_t again_ = kAgainAfter.size();
};

// An input history packet around a payload that is already encrypted: like the state's packet,
// with kind 1 and, in its fourth byte, the number of events less one.
[[nodiscard]] std::vector<std::uint8_t> encode_history_packet(std::uint16_t sequence, std::uint8_t events, std::span<const std::uint8_t> payload);

}  // namespace psvp::proto::transport
