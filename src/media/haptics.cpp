#include "media/haptics.h"

#include <algorithm>
#include <cstdlib>

namespace psvp::media {

namespace {
std::int16_t sample_at(std::span<const std::uint8_t> frame, std::size_t index, std::size_t channel) noexcept {
    const std::size_t at = (index * 2 + channel) * 2;
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(frame[at] | (frame[at + 1] << 8)));
}
constexpr std::size_t kFrameSamples = kHapticsFrameBytes / 4;
constexpr std::size_t kRightTriggerAt = 10, kLeftTriggerAt = 21;
constexpr std::uint8_t kChangesRightTrigger = 0x04, kChangesLeftTrigger = 0x08;
}  // namespace

std::optional<HapticsLevel> haptics_level(std::span<const std::uint8_t> frame) noexcept {
    if (frame.size() != kHapticsFrameBytes) return std::nullopt;
    int left = 0, right = 0;
    for (std::size_t i = 0; i < kFrameSamples; ++i) {
        left = std::max(left, std::abs(static_cast<int>(sample_at(frame, i, 0))));
        right = std::max(right, std::abs(static_cast<int>(sample_at(frame, i, 1))));
    }
    // 32768 and above count as the most.
    return HapticsLevel{static_cast<std::uint8_t>(std::min(left / 128, 255)), static_cast<std::uint8_t>(std::min(right / 128, 255))};
}

std::span<const std::int16_t> HapticsUpsampler::frame(std::span<const std::uint8_t> bytes) noexcept {
    if (bytes.size() != kHapticsFrameBytes) return {};
    std::size_t at = 0;
    for (std::size_t i = 0; i < kFrameSamples; ++i) {
        const int left = sample_at(bytes, i, 0), right = sample_at(bytes, i, 1);
        for (int step = 1; step <= static_cast<int>(kFactor); ++step) {
            out_[at++] = 0;  // the speaker's two channels
            out_[at++] = 0;
            out_[at++] = static_cast<std::int16_t>(last_left_ + (left - last_left_) * step / static_cast<int>(kFactor));
            out_[at++] = static_cast<std::int16_t>(last_right_ + (right - last_right_) * step / static_cast<int>(kFactor));
        }
        last_left_ = static_cast<std::int16_t>(left);
        last_right_ = static_cast<std::int16_t>(right);
    }
    return out_;
}

std::array<std::uint8_t, kDualSenseEffectsSize> dualsense_trigger_effects(std::uint8_t left_type, std::span<const std::uint8_t, 10> left,
                                                                         std::uint8_t right_type,
                                                                         std::span<const std::uint8_t, 10> right) noexcept {
    std::array<std::uint8_t, kDualSenseEffectsSize> report{};
    report[0] = kChangesRightTrigger | kChangesLeftTrigger;
    report[kRightTriggerAt] = right_type;
    std::copy(right.begin(), right.end(), report.begin() + kRightTriggerAt + 1);
    report[kLeftTriggerAt] = left_type;
    std::copy(left.begin(), left.end(), report.begin() + kLeftTriggerAt + 1);
    return report;
}

std::array<std::uint8_t, kDualSenseEffectsSize> dualsense_triggers_off() noexcept {
    constexpr std::array<std::uint8_t, 10> kNone{};
    return dualsense_trigger_effects(kTriggerEffectOff, kNone, kTriggerEffectOff, kNone);
}

}  // namespace psvp::media
