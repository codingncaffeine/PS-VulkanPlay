// What a console sends for the controller on a stream that is set up: how hard
// it is to rumble, what its adaptive triggers do, and its lights. Each comes as
// a data chunk of its own kind (transport.md); this reads and writes the
// chunks' content. docs/protocol/input.md, "What the console sends for the
// controller". No I/O in here.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace psvp::proto::transport {

// Data of kind 7: the two rumble motors, 0 (still) to 255.
struct Rumble {
    std::uint8_t first = 0;  // a byte whose meaning is not known
    std::uint8_t left = 0;   // the large motor, low and strong
    std::uint8_t right = 0;  // the small one, high and light
    friend bool operator==(const Rumble&, const Rumble&) = default;
};

// Data of kind 11: what the two adaptive triggers of a DualSense do, each as the type of
// effect and ten bytes of its parameters, the way the controller itself takes them.
struct TriggerEffects {
    std::uint8_t left_type = 0, right_type = 0;
    std::array<std::uint8_t, 10> left{}, right{};
    friend bool operator==(const TriggerEffects&, const TriggerEffects&) = default;
};

// How strong a console wants haptics or trigger effects: the numbers it sends.
enum class Strength : std::uint8_t { off = 0, strong = 1, medium = 2, weak = 3 };

// Data of kind 9: the controller's lights and settings.
struct ControllerInfo {
    std::uint8_t player = 0;               // which player's lights to show
    std::array<std::uint8_t, 3> light{};   // the light bar: red, green, blue
    bool reset_motion = false;             // take the controller's position now as where it rests
    std::uint8_t haptics = 0;              // a Strength, as sent
    std::uint8_t triggers = 0;
    // The long form names the input state it answers and when; the short form has neither.
    std::optional<std::uint16_t> answers;
    friend bool operator==(const ControllerInfo&, const ControllerInfo&) = default;
};

// What the stand-in console of the tests sends (ConsoleStream::Options::feedback_at).
inline constexpr Rumble kMockRumble{0, 200, 90};
inline constexpr TriggerEffects kMockTriggerEffects{0x21, 0x26, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10}, {11, 12, 13, 14, 15, 16, 17, 18, 19, 20}};
inline constexpr ControllerInfo kMockControllerInfo{2, {0, 64, 255}, false, 1, 2, std::uint16_t{0x1234}};

// Nothing when the content is too short, or (controller information) not one of its two sizes.
[[nodiscard]] std::optional<Rumble> parse_rumble(std::span<const std::uint8_t> content) noexcept;
[[nodiscard]] std::optional<TriggerEffects> parse_trigger_effects(std::span<const std::uint8_t> content) noexcept;
[[nodiscard]] std::optional<ControllerInfo> parse_controller_info(std::span<const std::uint8_t> content) noexcept;

// The other way round, for a console's side. Controller information is written in the long
// form (25 bytes) when it names an input state, and in the short one (17) otherwise.
[[nodiscard]] std::vector<std::uint8_t> encode_rumble(const Rumble& rumble);
[[nodiscard]] std::vector<std::uint8_t> encode_trigger_effects(const TriggerEffects& effects);
[[nodiscard]] std::vector<std::uint8_t> encode_controller_info(const ControllerInfo& info);

}  // namespace psvp::proto::transport
