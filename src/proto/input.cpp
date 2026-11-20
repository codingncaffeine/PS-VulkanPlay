#include "proto/input.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>

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

// ---- The input history ----------------------------------------------------

namespace {

constexpr std::array<std::string_view, kButtons> kButtonNames{"cross", "circle", "square", "triangle", "left", "right", "up", "down",
                                                              "l1", "r1", "l3", "r3", "options", "share", "touchpad", "ps"};
// The code of each button whose state travels in a third byte; 0 for those whose state is in the code.
constexpr std::array<std::uint8_t, kButtons> kStateByteCodes{0x88, 0x89, 0x8a, 0x8b, 0x82, 0x83, 0x80, 0x81, 0x84, 0x85, 0, 0, 0, 0, 0, 0};
// For the others: the code when released; pressed it is 0x20 more.
constexpr std::array<std::uint8_t, kButtons> kTwoByteCodes{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x8f, 0x90, 0x8c, 0x8d, 0x91, 0x8e};
constexpr std::uint8_t kPressedOffset = 0x20;
constexpr std::uint8_t kCodeL2 = 0x86;
constexpr std::uint8_t kCodeR2 = 0x87;

}  // namespace

std::string_view button_name(Button button) noexcept { return kButtonNames[static_cast<std::size_t>(button)]; }

std::optional<Button> button_named(std::string_view name) noexcept {
    for (std::size_t i = 0; i < kButtons; ++i) {
        if (kButtonNames[i] == name) return static_cast<Button>(i);
    }
    return std::nullopt;
}

HistoryEvent button_event(Button button, bool pressed) noexcept {
    const auto index = static_cast<std::size_t>(button);
    HistoryEvent event;
    event.bytes[0] = 0x80;
    if (kStateByteCodes[index] != 0) {
        event.bytes[1] = kStateByteCodes[index];
        event.bytes[2] = pressed ? 0xff : 0x00;
        event.size = 3;
    } else {
        event.bytes[1] = static_cast<std::uint8_t>(kTwoByteCodes[index] + (pressed ? kPressedOffset : 0));
        event.size = 2;
    }
    return event;
}

HistoryEvent trigger_event(bool right, std::uint8_t value) noexcept {
    HistoryEvent event;
    event.bytes = {0x80, right ? kCodeR2 : kCodeL2, value, 0, 0};
    event.size = 3;
    return event;
}

HistoryEvent touch_event(const Touch& touch) noexcept {
    HistoryEvent event;
    event.bytes = {static_cast<std::uint8_t>(touch.down ? 0xd0 : 0xc0), static_cast<std::uint8_t>(touch.id & 0x7f),
                   static_cast<std::uint8_t>(touch.x >> 4), static_cast<std::uint8_t>(((touch.x & 0xf) << 4) | ((touch.y >> 8) & 0xf)),
                   static_cast<std::uint8_t>(touch.y)};
    event.size = 5;
    return event;
}

std::optional<std::vector<ReadEvent>> parse_history_events(std::span<const std::uint8_t> payload) {
    std::vector<ReadEvent> events;
    std::size_t at = 0;
    while (at < payload.size()) {
        ReadEvent event;
        const std::uint8_t first = payload[at];
        if (first == 0xd0 || first == 0xc0) {
            if (payload.size() - at < 5) return std::nullopt;
            event.what = ReadEvent::What::touch;
            event.touch.down = first == 0xd0;
            event.touch.id = payload[at + 1];
            if (event.touch.id > 0x7f) return std::nullopt;
            event.touch.x = static_cast<std::uint16_t>((payload[at + 2] << 4) | (payload[at + 3] >> 4));
            event.touch.y = static_cast<std::uint16_t>(((payload[at + 3] & 0xf) << 8) | payload[at + 4]);
            at += 5;
        } else if (first == 0x80) {
            if (payload.size() - at < 2) return std::nullopt;
            const std::uint8_t code = payload[at + 1];
            bool known = false;
            for (std::size_t i = 0; i < kButtons && !known; ++i) {
                if (kStateByteCodes[i] != 0 && kStateByteCodes[i] == code) {
                    if (payload.size() - at < 3 || (payload[at + 2] != 0x00 && payload[at + 2] != 0xff)) return std::nullopt;
                    event.button = static_cast<Button>(i);
                    event.pressed = payload[at + 2] == 0xff;
                    at += 3;
                    known = true;
                } else if (kTwoByteCodes[i] != 0 && (kTwoByteCodes[i] == code || kTwoByteCodes[i] + kPressedOffset == code)) {
                    event.button = static_cast<Button>(i);
                    event.pressed = code != kTwoByteCodes[i];
                    at += 2;
                    known = true;
                }
            }
            if (!known && (code == kCodeL2 || code == kCodeR2)) {
                if (payload.size() - at < 3) return std::nullopt;
                event.what = ReadEvent::What::trigger;
                event.right = code == kCodeR2;
                event.value = payload[at + 2];
                at += 3;
                known = true;
            }
            if (!known) return std::nullopt;
        } else {
            return std::nullopt;
        }
        events.push_back(event);
    }
    return events;
}

std::string describe(const ReadEvent& event) {
    switch (event.what) {
    case ReadEvent::What::button: return std::string(button_name(event.button)) + (event.pressed ? " pressed" : " released");
    case ReadEvent::What::trigger: return std::string(event.right ? "r2 at " : "l2 at ") + std::to_string(event.value);
    case ReadEvent::What::touch:
        return "touch " + std::to_string(event.touch.id) + (event.touch.down ? " down at " : " up at ") + std::to_string(event.touch.x) + "," +
               std::to_string(event.touch.y);
    }
    return {};
}

std::vector<InputHistory::Packet> InputHistory::on_change(const Buttons& before, const Buttons& now, TimePoint when) {
    std::vector<Packet> packets;
    const auto push = [&](const HistoryEvent& event) {
        events_.insert(events_.begin(), event);
        if (events_.size() > kKept) events_.resize(kKept);
        Packet packet;
        packet.sequence = pushed_++;
        packet.events = static_cast<std::uint8_t>(events_.size());
        for (const HistoryEvent& kept : events_) packet.payload.insert(packet.payload.end(), kept.bytes.begin(), kept.bytes.begin() + kept.size);
        packets.push_back(std::move(packet));
        if (events_.size() > kKeptAfterSend) events_.resize(kKeptAfterSend);
    };
    const auto buttons = [&](bool pressed) {
        for (std::size_t i = 0; i < kButtons; ++i) {
            const auto button = static_cast<Button>(i);
            if (before.is_pressed(button) != now.is_pressed(button) && now.is_pressed(button) == pressed) push(button_event(button, pressed));
        }
    };
    // Releases go before touch changes: when a click of the touchpad and its finger are released
    // together, the finger must not lift while the click still counts as held.
    buttons(false);
    for (std::size_t i = 0; i < now.touches.size(); ++i) {
        const Touch& was = before.touches[i];
        const Touch& is = now.touches[i];
        if (was.down && (!is.down || is.id != was.id)) push(touch_event(Touch{false, was.id, was.x, was.y}));
        else if (is.down && is != was) push(touch_event(is));
    }
    buttons(true);
    if (before.l2 != now.l2) push(trigger_event(false, now.l2));
    if (before.r2 != now.r2) push(trigger_event(true, now.r2));
    if (!packets.empty()) {
        last_ = packets.back();
        last_sent_ = when;
        again_ = 0;
    }
    return packets;
}

std::optional<TimePoint> InputHistory::deadline() const {
    if (!last_ || again_ >= kAgainAfter.size()) return std::nullopt;
    return last_sent_ + kAgainAfter[again_];
}

std::optional<InputHistory::Packet> InputHistory::on_timeout(TimePoint now) {
    const auto due = deadline();
    if (!due || now < *due) return std::nullopt;
    ++again_;
    return last_;
}

std::vector<std::uint8_t> encode_history_packet(std::uint16_t sequence, std::uint8_t events, std::span<const std::uint8_t> payload) {
    std::vector<std::uint8_t> out(kInputHeaderSize, 0);
    out[0] = packet_kind::kInputHistory;
    out[1] = static_cast<std::uint8_t>(sequence >> 8);
    out[2] = static_cast<std::uint8_t>(sequence);
    out[3] = static_cast<std::uint8_t>(events == 0 ? 0 : events - 1);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

}  // namespace psvp::proto::transport
