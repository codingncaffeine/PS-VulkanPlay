// A DualSense's haptics, as a console streams them, and what the controller
// itself takes: frames of samples for its two actuators, brought to the rate
// of the controller's own sound device, and the report that sets what its
// adaptive triggers do. No device and no I/O in here.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace psvp::media {

// A haptics frame: 10 ms for two actuators, 30 samples each, 16 bits little-endian, left first.
inline constexpr std::size_t kHapticsFrameBytes = 120;
inline constexpr unsigned kHapticsRate = 3000;

// How far each actuator swings in a frame, 0 to 255: its largest sample. For a controller
// that can only rumble. Nothing for bytes that are not a haptics frame.
struct HapticsLevel {
    std::uint8_t left = 0, right = 0;
    friend bool operator==(const HapticsLevel&, const HapticsLevel&) = default;
};
[[nodiscard]] std::optional<HapticsLevel> haptics_level(std::span<const std::uint8_t> frame) noexcept;

// Brings haptics frames to the form a DualSense's sound device takes over USB: 48000 samples a
// second on four channels, of which the first two are its speaker (kept silent here) and the
// last two its actuators. Sixteen samples are drawn between every two of a frame, in a line.
class HapticsUpsampler {
public:
    static constexpr unsigned kRate = 48000, kChannels = 4, kFactor = kRate / kHapticsRate;
    static constexpr std::size_t kSamples = kHapticsFrameBytes / 4 * kFactor;  // per channel, per frame
    // One frame's samples, the channels interleaved; they stay valid until the next call. Empty
    // for bytes that are not a haptics frame.
    [[nodiscard]] std::span<const std::int16_t> frame(std::span<const std::uint8_t> bytes) noexcept;

private:
    std::int16_t last_left_ = 0, last_right_ = 0;
    std::array<std::int16_t, kSamples * kChannels> out_{};
};

// The report a DualSense takes its effects from, as SDL passes it on (SDL_SendGamepadEffect): 47
// bytes, of which the first says what the report changes (4: the right trigger, 8: the left
// one), bytes 10 to 20 are the right trigger's effect and bytes 21 to 31 the left one's, each a
// type and ten bytes of parameters. The layout is the one SDL's own test program documents.
inline constexpr std::size_t kDualSenseEffectsSize = 47;
inline constexpr std::uint8_t kTriggerEffectOff = 0x05;  // the type that clears a trigger's effect
[[nodiscard]] std::array<std::uint8_t, kDualSenseEffectsSize> dualsense_trigger_effects(std::uint8_t left_type, std::span<const std::uint8_t, 10> left,
                                                                                        std::uint8_t right_type,
                                                                                        std::span<const std::uint8_t, 10> right) noexcept;
// The same report with both triggers' effects cleared.
[[nodiscard]] std::array<std::uint8_t, kDualSenseEffectsSize> dualsense_triggers_off() noexcept;

}  // namespace psvp::media
