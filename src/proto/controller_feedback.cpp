#include "proto/controller_feedback.h"

#include <algorithm>

namespace psvp::proto::transport {

namespace {
constexpr std::size_t kRumbleSize = 3;
constexpr std::size_t kTriggerEffectsSize = 25;
constexpr std::size_t kInfoLong = 25, kInfoShort = 17;
// The long form of the controller information begins with eight bytes the short one lacks:
// the input state it answers (2), two bytes of unknown meaning, and a time (4).
constexpr std::size_t kInfoLead = kInfoLong - kInfoShort;
// Within the part both forms share.
constexpr std::size_t kPlayerAt = 0, kLightAt = 1, kResetAt = 4, kHapticsAt = 12, kTriggersAt = 13;
}  // namespace

std::optional<Rumble> parse_rumble(std::span<const std::uint8_t> content) noexcept {
    if (content.size() < kRumbleSize) return std::nullopt;
    return Rumble{content[0], content[1], content[2]};
}

std::optional<TriggerEffects> parse_trigger_effects(std::span<const std::uint8_t> content) noexcept {
    if (content.size() < kTriggerEffectsSize) return std::nullopt;
    TriggerEffects effects;
    effects.left_type = content[1];
    effects.right_type = content[2];
    std::copy_n(content.begin() + 5, 10, effects.left.begin());
    std::copy_n(content.begin() + 15, 10, effects.right.begin());
    return effects;
}

std::optional<ControllerInfo> parse_controller_info(std::span<const std::uint8_t> content) noexcept {
    if (content.size() != kInfoLong && content.size() != kInfoShort) return std::nullopt;
    ControllerInfo info;
    if (content.size() == kInfoLong) {
        info.answers = static_cast<std::uint16_t>((content[0] << 8) | content[1]);
        content = content.subspan(kInfoLead);
    }
    info.player = content[kPlayerAt];
    std::copy_n(content.begin() + kLightAt, 3, info.light.begin());
    info.reset_motion = content[kResetAt] != 0;
    info.haptics = content[kHapticsAt];
    info.triggers = content[kTriggersAt];
    return info;
}

std::vector<std::uint8_t> encode_rumble(const Rumble& rumble) { return {rumble.first, rumble.left, rumble.right}; }

std::vector<std::uint8_t> encode_trigger_effects(const TriggerEffects& effects) {
    std::vector<std::uint8_t> content(kTriggerEffectsSize, 0);
    content[1] = effects.left_type;
    content[2] = effects.right_type;
    std::copy(effects.left.begin(), effects.left.end(), content.begin() + 5);
    std::copy(effects.right.begin(), effects.right.end(), content.begin() + 15);
    return content;
}

std::vector<std::uint8_t> encode_controller_info(const ControllerInfo& info) {
    std::vector<std::uint8_t> content(info.answers ? kInfoLong : kInfoShort, 0);
    std::size_t base = 0;
    if (info.answers) {
        content[0] = static_cast<std::uint8_t>(*info.answers >> 8);
        content[1] = static_cast<std::uint8_t>(*info.answers & 0xff);
        base = kInfoLead;
    }
    content[base + kPlayerAt] = info.player;
    std::copy(info.light.begin(), info.light.end(), content.begin() + static_cast<std::ptrdiff_t>(base + kLightAt));
    content[base + kResetAt] = info.reset_motion ? 1 : 0;
    content[base + kHapticsAt] = info.haptics;
    content[base + kTriggersAt] = info.triggers;
    return content;
}

}  // namespace psvp::proto::transport
